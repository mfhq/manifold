/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-service.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include <manifold.h>
#include "mf-manager-interface.h"
#include "mf-remote-private.h"
#include "mf-sdautoptr-private.h"
#include "mf-trusted-keys-private.h"

#include "mfd-manager.h"
#include "mfd-policy.h"
#include "mf-varlink-util.h"

/* the time after which we exit if no connection is open, when we were activated on demand */
constexpr uint64_t IDLE_TIMEOUT_USEC = 30 * G_USEC_PER_SEC;

/**
 * MfdService:
 *
 * The service that manages the Manifold software installed on the host.
 * Every user can read the store directly, so for now the service only
 * handles requests that change it.
 *
 * The service speaks Varlink. A request that changes the installation needs
 * the authorization of polkit, see mfd_service_authorize(). These requests
 * run one at a time, in the order in which their callers were authorized.
 */
struct _MfdService {
    GObject parent_instance;

    MfdManager *manager;
    sd_varlink_server *server;
    sd_event_source *idle_timer; /* NULL unless we exit when idle */

    sd_bus *bus;                   /* the system bus, to ask polkit; NULL until we need it */
    GHashTable *requests;          /* maps a connection to its pending request, see MfdRequest */
    GQueue queue;                  /* the authorized requests, in the order in which they will run */
    sd_event_source *queue_runner; /* runs the request at the head of the queue */
};

G_DEFINE_TYPE(MfdService, mfd_service, G_TYPE_OBJECT)

typedef struct _MfdRequest MfdRequest;

/* Return value of a #MfdRequestFunc that passed its request to polkit again. The reply
 * functions of sd-varlink return small positive numbers, so we use a value that can not be
 * mistaken for one of them. */
#define MFD_REQUEST_DEFERRED INT_MAX

/**
 * MfdRequestFunc:
 *
 * Execute a request, and reply to its method call. The reply is an error
 * if the request failed.
 *
 * Returns: %MFD_REQUEST_DEFERRED if polkit has to authorize the request for
 *   a different action, in which case the request may be freed already.
 *   Otherwise a negative errno if we were unable to reply, or any other
 *   number if we replied.
 */
typedef int (*MfdRequestFunc)(MfdService *service, MfdRequest *request);

/**
 * MfdRequest:
 *
 * A method call that changes the installation. We reply to it later: first
 * polkit has to authorize the caller, then the request waits in the queue
 * until the requests before it are done.
 *
 * sd-varlink handles one call of a connection at a time, so a connection
 * has at most one request.
 */
struct _MfdRequest {
    MfdService *service;
    sd_varlink *link;
    MfdRequestFunc run;
    const char *method;       /* the method that was called, for the log */
    const char *action_id;    /* the polkit action that the caller needs */
    uid_t uid;                /* the calling user */
    gboolean report_progress; /* the call was made with the 'more' flag */
    MfTransactionFlags flags; /* the transaction flags of the call */
    sd_bus_slot *polkit_slot; /* the authorization check in progress, or NULL */

    int bundle_fd; /* the bundle to install, or -1 */
    MfdInstallSet *
        install_set; /* the components to install, set once the bundle was read or the request was resolved */
    MfdRepoRequest *repo_request; /* the request to install from the remotes, or NULL */
    char *component_id;           /* the component to remove or mark */
    char *version;                /* the version to remove or mark, or NULL for all of them */
    MfInstallMode mode;           /* the install mode to mark the component with */
    GStrv remote_ids;             /* the remotes to refresh, or NULL for all of them */
    char *url;                    /* the repository to inspect or add as a remote */
    char *public_key; /* the key that has to sign the repository, or NULL to use the key it serves */
    GStrv channels;   /* the channels of the remote to add, or NULL for the default one */
    char *remote_id;  /* the remote to remove */
};

/**
 * mfd_request_new:
 * @link: The connection that the method was called on.
 * @run: The function that executes the request.
 * @method: The method that was called.
 * @action_id: The polkit action that the caller needs.
 * @flags: The flags of the method call.
 *
 * Create a request for a method call. The caller sets the parameters of the
 * request, and then passes it to mfd_service_authorize().
 */
static MfdRequest *
mfd_request_new(sd_varlink *link,
                MfdRequestFunc run,
                const char *method,
                const char *action_id,
                sd_varlink_method_flags_t flags)
{
    MfdRequest *request = g_new0(MfdRequest, 1);

    request->link = sd_varlink_ref(link);
    request->run = run;
    request->method = method;
    request->action_id = action_id;
    request->report_progress = (flags & SD_VARLINK_METHOD_MORE) != 0;
    request->bundle_fd = -1;

    return request;
}

static void
mfd_request_free(MfdRequest *request)
{
    sd_bus_slot_unref(request->polkit_slot);
    sd_varlink_unref(request->link);
    g_clear_pointer(&request->install_set, mfd_install_set_free);
    g_clear_fd(&request->bundle_fd, NULL);
    g_clear_pointer(&request->repo_request, mfd_repo_request_free);
    g_free(request->component_id);
    g_free(request->version);
    g_strfreev(request->remote_ids);
    g_free(request->url);
    g_free(request->public_key);
    g_strfreev(request->channels);
    g_free(request->remote_id);
    g_free(request);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRequest, mfd_request_free)

static void
mfd_service_finalize(GObject *object)
{
    MfdService *service = MFD_SERVICE(object);

    sd_event_source_unref(service->queue_runner);
    g_queue_clear(&service->queue);
    g_hash_table_unref(service->requests);
    sd_bus_flush_close_unref(service->bus);
    sd_event_source_unref(service->idle_timer);
    sd_varlink_server_unref(service->server);
    g_clear_object(&service->manager);

    G_OBJECT_CLASS(mfd_service_parent_class)->finalize(object);
}

static void
mfd_service_class_init(MfdServiceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mfd_service_finalize;
}

static void
mfd_service_init(MfdService *service)
{
    service->requests = g_hash_table_new_full(g_direct_hash,
                                              g_direct_equal,
                                              NULL,
                                              (GDestroyNotify) mfd_request_free);
    g_queue_init(&service->queue);
}

/**
 * mfd_json_dispatch_transaction_flags:
 *
 * Read the "flags" parameter of a call into #MfTransactionFlags.
 * Unknown flags are rejected.
 */
static int
mfd_json_dispatch_transaction_flags(const char *name,
                                    sd_json_variant *variant,
                                    sd_json_dispatch_flags_t flags,
                                    void *userdata)
{
    MfTransactionFlags *ret = userdata;

    *ret = MF_TRANSACTION_FLAG_NONE;
    if (sd_json_variant_is_null(variant))
        return 0;
    if (!sd_json_variant_is_array(variant))
        return -EINVAL;

    for (size_t i = 0; i < sd_json_variant_elements(variant); i++) {
        sd_json_variant *flag = sd_json_variant_by_index(variant, i);

        if (!sd_json_variant_is_string(flag))
            return -EINVAL;
        if (g_str_equal(sd_json_variant_string(flag), MF_VARLINK_TRANSACTION_FLAG_ALLOW_INTERACTIVE))
            mf_flags_add(*ret, MF_TRANSACTION_FLAG_INTERACTIVE);
        else
            return -EINVAL;
    }

    return 0;
}

/**
 * mfd_error_is_about_request:
 *
 * Returns: %TRUE if @error was caused by the request, %FALSE if it is a
 *   failure of the service
 */
static gboolean
mfd_error_is_about_request(const GError *error)
{
    if (error->domain != MF_ERROR)
        return FALSE;

    switch (error->code) {
    case MF_ERROR_NOT_FOUND:
    case MF_ERROR_PERMISSION_DENIED:
    case MF_ERROR_INVALID_BUNDLE:
    case MF_ERROR_WRONG_ARCHITECTURE:
    case MF_ERROR_VERIFICATION_FAILED:
    case MF_ERROR_MISSING_BASE:
    case MF_ERROR_ALREADY_INSTALLED:
    case MF_ERROR_COMPONENT_IN_USE:
    case MF_ERROR_INVALID_MANIFEST:
    case MF_ERROR_REVOKED:
    case MF_ERROR_ID_CONFLICT:
    case MF_ERROR_FILE_CONFLICT:
    case MF_ERROR_INVALID_REPOSITORY:
    case MF_ERROR_EXPIRED:
    case MF_ERROR_DOWNLOAD_FAILED:
    case MF_ERROR_AMBIGUOUS:
    case MF_ERROR_REMOTE_EXISTS:
        return TRUE;
    default:
        return FALSE;
    }
}

/**
 * mfd_service_reply_error:
 * @detail: (nullable): the details that the manager returned about the error
 *
 * Reply with the error of the interface that corresponds to a #GError.
 * The reply always has the message of @error, and the details of the
 * component it is about if @detail has them.
 *
 * Returns: The result of sd_varlink_error(), which is a negative errno
 *   even if the reply was sent.
 */
static int
mfd_service_reply_error(sd_varlink *link, const GError *error, const MfdErrorDetail *detail)
{
    MfError code = error->domain == MF_ERROR ? error->code : MF_ERROR_FAILED;
    const char *error_id = mf_error_to_varlink_id(code);

    /* we only warn about failures of our own, not about requests we can not fulfill */
    if (mfd_error_is_about_request(error))
        g_message("%s", error->message);
    else
        g_warning("%s", error->message);

    /* permission denied is a standard Varlink error, which has no parameters */
    if (code == MF_ERROR_PERMISSION_DENIED)
        return sd_varlink_error(link, error_id, NULL);

    if (detail != NULL && detail->component_id != NULL) {
        switch (code) {
        case MF_ERROR_MISSING_BASE:
            return sd_varlink_errorbo(link,
                                      error_id,
                                      SD_JSON_BUILD_PAIR_STRING("message", error->message),
                                      SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                                      SD_JSON_BUILD_PAIR_STRING("baseId", detail->base_id),
                                      SD_JSON_BUILD_PAIR_STRING("baseVersion", detail->base_version),
                                      SD_JSON_BUILD_PAIR_STRING("identity", detail->identity));
        case MF_ERROR_ALREADY_INSTALLED:
            return sd_varlink_errorbo(link,
                                      error_id,
                                      SD_JSON_BUILD_PAIR_STRING("message", error->message),
                                      SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                                      SD_JSON_BUILD_PAIR_STRING("version", detail->version));
        case MF_ERROR_COMPONENT_IN_USE:
            return sd_varlink_errorbo(link,
                                      error_id,
                                      SD_JSON_BUILD_PAIR_STRING("message", error->message),
                                      SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                                      SD_JSON_BUILD_PAIR_STRING("usedBy", detail->used_by));
        case MF_ERROR_NOT_FOUND:
            return sd_varlink_errorbo(link,
                                      error_id,
                                      SD_JSON_BUILD_PAIR_STRING("message", error->message),
                                      SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                                      SD_JSON_BUILD_PAIR_CONDITION(detail->version != NULL,
                                                                   "version",
                                                                   SD_JSON_BUILD_STRING(detail->version)));
        case MF_ERROR_WRONG_ARCHITECTURE:
            return sd_varlink_errorbo(
                link,
                error_id,
                SD_JSON_BUILD_PAIR_STRING("message", error->message),
                SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                SD_JSON_BUILD_PAIR_STRING("architecture", detail->architecture),
                SD_JSON_BUILD_PAIR_STRING("hostArchitecture", mf_get_host_architecture()));
        case MF_ERROR_ID_CONFLICT:
            return sd_varlink_errorbo(
                link,
                error_id,
                SD_JSON_BUILD_PAIR_STRING("message", error->message),
                SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                SD_JSON_BUILD_PAIR_STRING("kind", mf_component_kind_to_string(detail->kind)),
                SD_JSON_BUILD_PAIR_BOOLEAN("sdk", detail->sdk));
        default:
            break;
        }
    }

    return sd_varlink_errorbo(link, error_id, SD_JSON_BUILD_PAIR_STRING("message", error->message));
}

/**
 * mfd_varlink_append_component:
 *
 * Add an installed component to the array of a reply.
 */
static int
mfd_varlink_append_component(sd_json_variant **array, MfComponent *component)
{
    return sd_json_variant_append_arraybo(
        array,
        SD_JSON_BUILD_PAIR_STRING("componentId", mf_component_get_id(component)),
        SD_JSON_BUILD_PAIR_STRING("kind", mf_component_kind_to_string(mf_component_get_kind(component))),
        SD_JSON_BUILD_PAIR_STRING("version", mf_component_get_version(component)),
        SD_JSON_BUILD_PAIR_UNSIGNED("revision", mf_component_get_revision(component)),
        SD_JSON_BUILD_PAIR_BOOLEAN("sdk", mf_component_is_sdk(component)),
        SD_JSON_BUILD_PAIR_STRING("identity", mf_component_get_identity(component)),
        SD_JSON_BUILD_PAIR_STRING("verityHash", mf_component_get_verity_hash(component)),
        SD_JSON_BUILD_PAIR_BOOLEAN("current", mf_component_is_current(component)),
        SD_JSON_BUILD_PAIR_STRING("installMode",
                                  mf_install_mode_to_string(mf_component_get_install_mode(component))),
        SD_JSON_BUILD_PAIR_STRING("origin", mf_component_get_origin(component)));
}

/**
 * mfd_json_ensure_array:
 *
 * Create an empty JSON array if @array is %NULL. An array that nothing was
 * appended to is still %NULL, but a reply needs an empty array in this case.
 *
 * Returns: 0, or a negative errno
 */
static int
mfd_json_ensure_array(sd_json_variant **array)
{
    if (*array != NULL)
        return 0;
    return sd_json_variant_new_array(array, NULL, 0);
}

/**
 * mfd_varlink_build_components_array:
 *
 * Build the array of components of a reply.
 *
 * Returns: 0, or a negative errno
 */
static int
mfd_varlink_build_components_array(GPtrArray *components, sd_json_variant **ret_array)
{
    g_autoptr(sd_json_variant) array = NULL;
    int r;

    for (guint i = 0; i < components->len; i++) {
        if ((r = mfd_varlink_append_component(&array, g_ptr_array_index(components, i))) < 0)
            return r;
    }
    if ((r = mfd_json_ensure_array(&array)) < 0)
        return r;

    *ret_array = g_steal_pointer(&array);
    return 0;
}

/**
 * mfd_varlink_build_strings_array:
 * @strings: (element-type utf8): the strings to put into the array
 * @ret: (out): the JSON array
 *
 * Returns: 0, or a negative errno
 */
static int
mfd_varlink_build_strings_array(GPtrArray *strings, sd_json_variant **ret)
{
    int r;

    for (guint i = 0; i < strings->len; i++) {
        r = sd_json_variant_append_arrayb(ret, SD_JSON_BUILD_STRING(g_ptr_array_index(strings, i)));
        if (r < 0)
            return r;
    }

    return mfd_json_ensure_array(ret);
}

/**
 * mfd_varlink_build_updates_array:
 * @updates: (element-type MfUpdate): the components that an operation updated
 * @ret_array: (out): the JSON array
 *
 * Returns: 0, or a negative errno
 */
static int
mfd_varlink_build_updates_array(GPtrArray *updates, sd_json_variant **ret_array)
{
    g_autoptr(sd_json_variant) array = NULL;
    int r;

    for (guint i = 0; i < updates->len; i++) {
        MfUpdate *update = g_ptr_array_index(updates, i);
        g_autoptr(sd_json_variant) components = NULL;
        g_autoptr(sd_json_variant) added = NULL;
        g_autoptr(sd_json_variant) removed = NULL;

        if ((r = mfd_varlink_append_component(&components, mf_update_get_component(update))) < 0)
            return r;
        if ((r = mfd_varlink_build_strings_array(mf_update_get_integration_added(update), &added)) < 0)
            return r;
        if ((r = mfd_varlink_build_strings_array(mf_update_get_integration_removed(update), &removed)) < 0)
            return r;
        r = sd_json_variant_append_arraybo(
            &array,
            SD_JSON_BUILD_PAIR_VARIANT("component", sd_json_variant_by_index(components, 0)),
            SD_JSON_BUILD_PAIR_STRING("previousVersion", mf_update_get_previous_version(update)),
            SD_JSON_BUILD_PAIR_VARIANT("integrationAdded", added),
            SD_JSON_BUILD_PAIR_VARIANT("integrationRemoved", removed));
        if (r < 0)
            return r;
    }
    if ((r = mfd_json_ensure_array(&array)) < 0)
        return r;

    *ret_array = g_steal_pointer(&array);
    return 0;
}

/**
 * mfd_service_reply_result:
 *
 * Send the final reply of an operation: the components it updated, installed
 * and removed, the images it removed, the disk space it used and freed, and
 * its warnings.
 */
static int
mfd_service_reply_result(sd_varlink *link, MfResult *result)
{
    g_autoptr(sd_json_variant) updated = NULL;
    g_autoptr(sd_json_variant) installed = NULL;
    g_autoptr(sd_json_variant) removed = NULL;
    g_autoptr(sd_json_variant) removed_images = NULL;
    g_autoptr(sd_json_variant) warnings = NULL;
    int r;

    if ((r = mfd_varlink_build_updates_array(mf_result_get_updated(result), &updated)) < 0)
        return r;
    if ((r = mfd_varlink_build_components_array(mf_result_get_installed(result), &installed)) < 0)
        return r;
    if ((r = mfd_varlink_build_components_array(mf_result_get_removed(result), &removed)) < 0)
        return r;
    if ((r = mfd_varlink_build_strings_array(mf_result_get_removed_images(result), &removed_images)) < 0)
        return r;
    if ((r = mfd_varlink_build_strings_array(mf_result_get_warnings(result), &warnings)) < 0)
        return r;

    return sd_varlink_replybo(
        link,
        SD_JSON_BUILD_PAIR(
            "result",
            SD_JSON_BUILD_OBJECT(SD_JSON_BUILD_PAIR_VARIANT("updated", updated),
                                 SD_JSON_BUILD_PAIR_VARIANT("installed", installed),
                                 SD_JSON_BUILD_PAIR_VARIANT("removed", removed),
                                 SD_JSON_BUILD_PAIR_VARIANT("removedImages", removed_images),
                                 SD_JSON_BUILD_PAIR_UNSIGNED("bytesAdded", mf_result_get_bytes_added(result)),
                                 SD_JSON_BUILD_PAIR_UNSIGNED("bytesFreed", mf_result_get_bytes_freed(result)),
                                 SD_JSON_BUILD_PAIR_VARIANT("warnings", warnings))));
}

/**
 * mfd_varlink_can_write:
 *
 * Check whether the socket of a connection can take more data right now.
 */
static gboolean
mfd_varlink_can_write(sd_varlink *link)
{
    struct pollfd pfd = {.fd = sd_varlink_get_fd(link), .events = POLLOUT};

    return pfd.fd >= 0 && poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP)) == POLLOUT;
}

/**
 * mfd_service_on_progress:
 *
 * Send the progress of the operation to the caller.
 */
static void
mfd_service_on_progress(const MfProgress *progress, gpointer user_data)
{
    sd_varlink *link = user_data;
    const MfItemProgress *item = mf_progress_get_item(progress);
    const char *action = item != NULL ? mf_action_to_string(mf_item_progress_get_action(item)) : NULL;
    const char *subject = item != NULL ? mf_item_progress_get_subject(item) : NULL;
    const char *version = item != NULL ? mf_item_progress_get_version(item) : NULL;
    double item_percent = item != NULL ? mf_item_progress_get_percent(item) : 0;
    uint64_t bytes_done = item != NULL ? mf_item_progress_get_bytes_done(item) : 0;
    uint64_t bytes_total = item != NULL ? mf_item_progress_get_bytes_total(item) : 0;

    /* NOTE: The operation blocks the event loop, so we never wait for a caller that does
     * not read its progress. We drop the progress while the socket of the caller is full. */
    if (!mfd_varlink_can_write(link))
        return;

    int r = sd_varlink_notifybo(
        link,
        SD_JSON_BUILD_PAIR("progress",
                           SD_JSON_BUILD_OBJECT(
                               SD_JSON_BUILD_PAIR_REAL("percent", mf_progress_get_percent(progress)),
                               SD_JSON_BUILD_PAIR_CONDITION(
                                   item != NULL,
                                   "item",
                                   SD_JSON_BUILD_OBJECT(
                                       SD_JSON_BUILD_PAIR_STRING("action", action),
                                       SD_JSON_BUILD_PAIR_STRING("subject", subject),
                                       SD_JSON_BUILD_PAIR_CONDITION(version != NULL,
                                                                    "version",
                                                                    SD_JSON_BUILD_STRING(version)),
                                       SD_JSON_BUILD_PAIR_REAL("percent", item_percent),
                                       SD_JSON_BUILD_PAIR_CONDITION(bytes_total > 0,
                                                                    "bytesDone",
                                                                    SD_JSON_BUILD_UNSIGNED(bytes_done)),
                                       SD_JSON_BUILD_PAIR_CONDITION(bytes_total > 0,
                                                                    "bytesTotal",
                                                                    SD_JSON_BUILD_UNSIGNED(bytes_total)))))));
    /* sd-varlink only queues the notification, and we are busy, so we write it out now */
    if (r >= 0)
        r = sd_varlink_flush(link);
    if (r < 0)
        g_warning("Unable to report progress: %s", g_strerror(-r));
}

/**
 * mfd_service_remove:
 *
 * Remove the component of the request, or clean up if the request names
 * no component, and reply to the call.
 */
static int
mfd_service_remove(MfdService *service, MfdRequest *request)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    if (request->component_id == NULL)
        g_message("User %u requested to clean up", (guint) request->uid);
    else
        g_message("User %u requested to remove %s %s",
                  (guint) request->uid,
                  request->component_id,
                  request->version != NULL ? request->version : "");

    result = mfd_manager_remove(service->manager,
                                request->component_id,
                                request->version,
                                request->report_progress ? mfd_service_on_progress : NULL,
                                request->link,
                                &detail,
                                &error);
    if (result == NULL) {
        mfd_service_reply_error(request->link, error, &detail);
        return 0;
    }

    return mfd_service_reply_result(request->link, result);
}

/**
 * mfd_service_refresh:
 *
 * Refresh the remotes of the request, and reply to the call.
 */
static int
mfd_service_refresh(MfdService *service, MfdRequest *request)
{
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    g_message("User %u requested to refresh %s",
              (guint) request->uid,
              request->remote_ids != NULL ? "some remotes" : "all remotes");

    result = mfd_manager_refresh(service->manager,
                                 (const char *const *) request->remote_ids,
                                 request->report_progress ? mfd_service_on_progress : NULL,
                                 request->link,
                                 &error);
    if (result == NULL) {
        mfd_service_reply_error(request->link, error, NULL);
        return 0;
    }

    return mfd_service_reply_result(request->link, result);
}

/**
 * mfd_service_inspect_remote:
 *
 * Inspect the repository of the request, and reply to the call with the
 * description of the repository and the key that signs it.
 */
static int
mfd_service_inspect_remote(MfdService *service, MfdRequest *request)
{
    g_autoptr(MfdRemoteInspection) inspection = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) channels = NULL;
    g_autofree char *valid_until = NULL;
    MfRepoRootIndex *index;

    g_message("User %u requested to inspect the repository at %s", (guint) request->uid, request->url);

    inspection = mfd_manager_inspect_remote(service->manager, request->url, request->public_key, &error);
    if (inspection == NULL) {
        mfd_service_reply_error(request->link, error, NULL);
        return 0;
    }
    index = inspection->index;
    channels = mfd_remote_inspection_list_channels(inspection);
    valid_until = g_date_time_format_iso8601(index->valid_until);

    return sd_varlink_replybo(
        request->link,
        SD_JSON_BUILD_PAIR(
            "remote",
            SD_JSON_BUILD_OBJECT(SD_JSON_BUILD_PAIR_STRING("id", index->id),
                                 SD_JSON_BUILD_PAIR_STRING("url", inspection->url),
                                 SD_JSON_BUILD_PAIR_STRING("summary", index->summary),
                                 SD_JSON_BUILD_PAIR_CONDITION(index->publisher_name != NULL,
                                                              "publisherName",
                                                              SD_JSON_BUILD_STRING(index->publisher_name)),
                                 SD_JSON_BUILD_PAIR_CONDITION(index->publisher_email != NULL,
                                                              "publisherEmail",
                                                              SD_JSON_BUILD_STRING(index->publisher_email)),
                                 SD_JSON_BUILD_PAIR_STRING("validUntil", valid_until),
                                 SD_JSON_BUILD_PAIR("channels", SD_JSON_BUILD_STRV(channels)),
                                 SD_JSON_BUILD_PAIR_STRING("publicKey", inspection->public_key),
                                 SD_JSON_BUILD_PAIR_STRING("fingerprint", index->key_fingerprint))));
}

/**
 * mfd_service_add_remote:
 *
 * Add the repository of the request as a remote, refresh it, and reply to the call.
 */
static int
mfd_service_add_remote(MfdService *service, MfdRequest *request)
{
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    g_message("User %u requested to add the repository at %s as a remote",
              (guint) request->uid,
              request->url);

    result = mfd_manager_add_remote(service->manager,
                                    request->remote_id,
                                    request->url,
                                    request->public_key,
                                    (const char *const *) request->channels,
                                    request->report_progress ? mfd_service_on_progress : NULL,
                                    request->link,
                                    &error);
    if (result == NULL) {
        mfd_service_reply_error(request->link, error, NULL);
        return 0;
    }

    return mfd_service_reply_result(request->link, result);
}

/**
 * mfd_service_remove_remote:
 *
 * Remove the remote of the request, and reply to the call.
 */
static int
mfd_service_remove_remote(MfdService *service, MfdRequest *request)
{
    g_autoptr(GError) error = NULL;

    g_message("User %u requested to remove the remote %s", (guint) request->uid, request->remote_id);

    if (!mfd_manager_remove_remote(service->manager, request->remote_id, &error)) {
        mfd_service_reply_error(request->link, error, NULL);
        return 0;
    }

    return sd_varlink_reply(request->link, NULL);
}

/**
 * mfd_service_mark:
 *
 * Mark the component of the request, and reply to the call.
 */
static int
mfd_service_mark(MfdService *service, MfdRequest *request)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(GPtrArray) marked = NULL;
    g_autoptr(sd_json_variant) components = NULL;
    g_autoptr(GError) error = NULL;
    int r;

    g_message("User %u requested to mark %s %s as %s",
              (guint) request->uid,
              request->component_id,
              request->version != NULL ? request->version : "",
              mf_install_mode_to_string(request->mode));

    marked = mfd_manager_mark(service->manager,
                              request->component_id,
                              request->version,
                              request->mode,
                              &detail,
                              &error);
    if (marked == NULL) {
        mfd_service_reply_error(request->link, error, &detail);
        return 0;
    }
    if ((r = mfd_varlink_build_components_array(marked, &components)) < 0)
        return r;

    return sd_varlink_replybo(
        request->link,
        SD_JSON_BUILD_PAIR("done",
                           SD_JSON_BUILD_OBJECT(SD_JSON_BUILD_PAIR_VARIANT("components", components))));
}

/**
 * mfd_service_drop_request:
 *
 * Forget a request, whether it is waiting for polkit or in the queue.
 */
static void
mfd_service_drop_request(MfdService *service, MfdRequest *request)
{
    g_queue_remove(&service->queue, request);
    g_hash_table_remove(service->requests, request->link);
}

/**
 * mfd_service_queue_request:
 *
 * Add an authorized request to the end of the queue.
 */
static void
mfd_service_queue_request(MfdService *service, MfdRequest *request)
{
    g_queue_push_tail(&service->queue, request);
    sd_event_source_set_enabled(service->queue_runner, SD_EVENT_ON);
}

/**
 * mfd_service_on_run_queue:
 *
 * Run the request at the head of the queue. We run one request per iteration
 * of the event loop, so new calls and the answers of polkit are handled
 * between two requests.
 */
static int
mfd_service_on_run_queue(sd_event_source *source, void *userdata)
{
    MfdService *service = userdata;
    MfdRequest *request = g_queue_pop_head(&service->queue);
    int r;

    if (g_queue_is_empty(&service->queue))
        sd_event_source_set_enabled(source, SD_EVENT_OFF);
    if (request == NULL)
        return 0;

    r = request->run(service, request);
    if (r == MFD_REQUEST_DEFERRED)
        return 0;
    if (r < 0) {
        g_warning("Unable to reply to the call of %s by user %u: %s",
                  request->method,
                  (guint) request->uid,
                  g_strerror(-r));
        /* reply with a generic error, so the caller learns that the call failed even if
         * building the reply was the step that failed */
        sd_varlink_error_errno(request->link, r);
    }
    g_hash_table_remove(service->requests, request->link);

    return 0;
}

/**
 * mfd_service_refuse_request:
 * @error: (nullable): Why polkit could not be asked, or %NULL if polkit denied the request.
 *
 * Reply with an error to a request that was not authorized, and drop the request.
 *
 * Returns: The result of sd_varlink_error(), which is a negative errno
 *   even if the reply was sent.
 */
static int
mfd_service_refuse_request(MfdService *service, MfdRequest *request, const GError *error)
{
    int r;

    if (error != NULL) {
        g_warning("Unable to authorize user %u for %s: %s",
                  (guint) request->uid,
                  request->method,
                  error->message);
        r = sd_varlink_errorbo(request->link,
                               mf_error_to_varlink_id(MF_ERROR_FAILED),
                               SD_JSON_BUILD_PAIR_STRING("message", error->message));
    } else {
        g_message("Denied call of %s by user %u: not authorized for %s",
                  request->method,
                  (guint) request->uid,
                  request->action_id);
        r = sd_varlink_error(request->link, SD_VARLINK_ERROR_PERMISSION_DENIED, NULL);
    }
    mfd_service_drop_request(service, request);

    return r;
}

/**
 * mfd_service_on_polkit_reply:
 *
 * Queue a request once polkit has authorized its caller, or refuse it.
 */
static int
mfd_service_on_polkit_reply(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    MfdRequest *request = userdata;
    MfdService *service = request->service;
    g_autoptr(GError) error = NULL;
    int r;

    request->polkit_slot = sd_bus_slot_unref(request->polkit_slot);

    r = mfd_policy_read_reply(reply, &error);
    if (r > 0) {
        g_message("User %u is authorized for %s", (guint) request->uid, request->action_id);
        mfd_service_queue_request(service, request);
        return 0;
    }

    mfd_service_refuse_request(service, request, error);
    return 0;
}

/**
 * mfd_service_get_bus:
 *
 * Returns: (transfer none) (nullable): The system bus, which we connect to
 *   when we need it first, or %NULL on error.
 */
static sd_bus *
mfd_service_get_bus(MfdService *service, GError **error)
{
    /* if the bus was closed, e.g. because the bus daemon was restarted, we connect again */
    if (service->bus != NULL && sd_bus_is_open(service->bus) <= 0)
        service->bus = sd_bus_flush_close_unref(service->bus);
    if (service->bus == NULL)
        service->bus = mfd_policy_connect(sd_varlink_server_get_event(service->server), error);

    return service->bus;
}

/**
 * mfd_service_ask_polkit:
 * @request: A request whose caller is not authorized for its action yet.
 *
 * Ask polkit whether the caller of @request is authorized for its action. We
 * queue the request once polkit has authorized the caller, see
 * mfd_service_on_polkit_reply().
 *
 * Returns: 0, or a negative errno if we refused the request right away.
 */
static int
mfd_service_ask_polkit(MfdService *service, MfdRequest *request)
{
    g_autoptr(GError) error = NULL;
    sd_bus *bus;

    bus = mfd_service_get_bus(service, &error);
    if (bus == NULL ||
        !mfd_policy_check_async(bus,
                                request->link,
                                request->uid,
                                request->action_id,
                                mf_flags_contains(request->flags, MF_TRANSACTION_FLAG_INTERACTIVE),
                                mfd_service_on_polkit_reply,
                                request,
                                &request->polkit_slot,
                                &error))
        return mfd_service_refuse_request(service, request, error);

    return 0;
}

/**
 * mfd_service_authorize:
 * @request: (transfer full): A request whose caller is not authorized yet.
 * @flags: The transaction flags of the call.
 *
 * Check whether the caller may make @request. Root and the user we are
 * running as may do everything, so we queue their requests right away.
 * For everyone else we ask polkit, see mfd_service_ask_polkit().
 *
 * The method callback has to validate the parameters of the call first, so
 * polkit does not ask the user for a password for a malformed call.
 *
 * Returns: 0, or a negative errno to return from the method callback.
 */
static int
mfd_service_authorize(MfdService *service, MfdRequest *request, MfTransactionFlags flags)
{
    int r;

    request->service = service;
    request->flags = flags;
    g_hash_table_insert(service->requests, request->link, request);

    r = mf_varlink_get_peer_uid(request->link, &request->uid);
    if (r < 0) {
        mfd_service_drop_request(service, request);
        return r;
    }

    if (mf_peer_is_privileged(request->uid)) {
        mfd_service_queue_request(service, request);
        return 0;
    }

    return mfd_service_ask_polkit(service, request);
}

/**
 * mfd_service_install_bundle:
 *
 * Install all components of a bundle, and reply to the call.
 *
 * We only know whether a bundle has a privileged component after reading it,
 * and we only read a bundle for a caller that is authorized to install bundles.
 * A bundle with a privileged component therefore needs a second authorization:
 * we pass the request to polkit again with the stricter action, and queue it
 * again once polkit has authorized the caller. Root and the user we are
 * running as are authorized for every action.
 */
static int
mfd_service_install_bundle(MfdService *service, MfdRequest *request)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    if (request->install_set == NULL) {
        g_autoptr(MfBundle) bundle = NULL;

        g_message("User %u requested to install a bundle", (guint) request->uid);

        bundle = mf_bundle_new_from_fd(request->bundle_fd, &error);
        if (bundle == NULL) {
            mfd_service_reply_error(request->link, error, NULL);
            return 0;
        }

        /* TODO: Verify the signature against trusted keys, once a host has some. */
        if (mf_bundle_is_signed(bundle))
            g_warning(
                "The bundle is signed, but the signature was not verified: trusted keys are not supported yet");
        else
            g_warning("The bundle is not signed");

        request->install_set = mfd_install_set_new_for_bundle(bundle);
        if (!mf_peer_is_privileged(request->uid) && mfd_install_set_is_privileged(request->install_set)) {
            request->action_id = MFD_POLKIT_ACTION_INSTALL_PRIVILEGED;
            mfd_service_ask_polkit(service, request);
            return MFD_REQUEST_DEFERRED;
        }
    }

    result = mfd_manager_install_set(service->manager,
                                     request->install_set,
                                     request->report_progress ? mfd_service_on_progress : NULL,
                                     request->link,
                                     &detail,
                                     &error);
    if (result == NULL) {
        mfd_service_reply_error(request->link, error, &detail);
        return 0;
    }

    return mfd_service_reply_result(request->link, result);
}

/**
 * mfd_service_needs_privileged:
 *
 * Check whether a request still needs the second authorization from polkit.
 * An update only needs the second authorization if a component becomes
 * privileged with the update. A component that is privileged already was
 * approved by an admin when it was installed.
 *
 * Returns: %TRUE if the request needs the second authorization
 */
static gboolean
mfd_service_needs_privileged(MfdService *service, MfdRequest *request)
{
    if (mf_peer_is_privileged(request->uid) ||
        g_str_equal(request->action_id, MFD_POLKIT_ACTION_INSTALL_PRIVILEGED))
        return FALSE;
    /* a request without a component to install is an update */
    if (request->repo_request->component_id == NULL)
        return mfd_manager_install_set_gains_privileges(service->manager, request->install_set);

    return mfd_install_set_is_privileged(request->install_set);
}

/**
 * mfd_service_install_from_remotes:
 *
 * Install the components of the request from the remotes, with the bases
 * they need, and reply to the call.
 *
 * Like for bundles, a privileged component needs a second authorization.
 * We can only ask polkit for it once the request is resolved, as we need
 * the manifest of the component to know whether it is privileged.
 *
 * If the installation fails because a repository has changed since the last
 * refresh, we refresh the remotes once and resolve the request again. The
 * new install set is then checked for privileged components again.
 */
static int
mfd_service_install_from_remotes(MfdService *service, MfdRequest *request)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    for (;;) {
        if (request->install_set == NULL) {
            request->install_set = mfd_manager_resolve(service->manager, request->repo_request, &error);
            if (request->install_set == NULL) {
                mfd_service_reply_error(request->link, error, NULL);
                return 0;
            }
            if (mfd_service_needs_privileged(service, request)) {
                request->action_id = MFD_POLKIT_ACTION_INSTALL_PRIVILEGED;
                mfd_service_ask_polkit(service, request);
                return MFD_REQUEST_DEFERRED;
            }
        }

        result = mfd_manager_install_set(service->manager,
                                         request->install_set,
                                         request->report_progress ? mfd_service_on_progress : NULL,
                                         request->link,
                                         &detail,
                                         &error);
        if (result != NULL)
            return mfd_service_reply_result(request->link, result);
        if (!mfd_manager_refresh_for_retry(service->manager,
                                           request->repo_request,
                                           request->install_set,
                                           error)) {
            mfd_service_reply_error(request->link, error, &detail);
            return 0;
        }

        g_clear_pointer(&request->install_set, mfd_install_set_free);
        g_clear_error(&error);
        mfd_error_detail_clear(&detail);
    }
}

/**
 * mfd_service_install_component:
 *
 * Install a component from the remotes, and reply to the call.
 */
static int
mfd_service_install_component(MfdService *service, MfdRequest *request)
{
    MfdRepoRequest *repo_request = request->repo_request;

    if (request->install_set == NULL)
        g_message("User %u requested to install %s%s%s",
                  (guint) request->uid,
                  repo_request->component_id,
                  repo_request->origin != NULL ? " from " : "",
                  repo_request->origin != NULL ? repo_request->origin : "");

    return mfd_service_install_from_remotes(service, request);
}

/**
 * mfd_service_update:
 *
 * Update the components of the request from their remotes, and reply to the call.
 */
static int
mfd_service_update(MfdService *service, MfdRequest *request)
{
    if (request->install_set == NULL)
        g_message("User %u requested to update %s",
                  (guint) request->uid,
                  request->repo_request->update_ids != NULL ? "some components" : "all components");

    return mfd_service_install_from_remotes(service, request);
}

/**
 * mfd_json_variant_get_strv:
 * @variant: (nullable): a parameter of a call
 * @ret_strv: (out) (transfer full): the strings of @variant, or %NULL if it is not set
 *
 * Returns: %TRUE if @variant is an array of strings, or is not set
 */
static gboolean
mfd_json_variant_get_strv(sd_json_variant *variant, GStrv *ret_strv)
{
    g_autoptr(GStrvBuilder) builder = NULL;

    *ret_strv = NULL;
    if (variant == NULL || sd_json_variant_is_null(variant))
        return TRUE;
    if (!sd_json_variant_is_array(variant))
        return FALSE;

    builder = g_strv_builder_new();
    for (size_t i = 0; i < sd_json_variant_elements(variant); i++) {
        sd_json_variant *entry = sd_json_variant_by_index(variant, i);

        if (!sd_json_variant_is_string(entry))
            return FALSE;
        g_strv_builder_add(builder, sd_json_variant_string(entry));
    }
    *ret_strv = g_strv_builder_end(builder);

    return TRUE;
}

/**
 * mfd_varlink_method_install_file:
 */
static int
mfd_varlink_method_install_file(sd_varlink *link,
                                sd_json_variant *parameters,
                                sd_varlink_method_flags_t flags,
                                void *userdata)
{
    MfdService *service = userdata;
    struct {
        guint fd_index;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"bundleFileDescriptor",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint,
         offsetof(typeof(p), fd_index),
         SD_JSON_MANDATORY},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    g_autoptr(MfdRequest) request = NULL;
    struct stat st;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    request = mfd_request_new(link,
                              mfd_service_install_bundle,
                              MF_VARLINK_METHOD_INSTALL_FILE,
                              MFD_POLKIT_ACTION_INSTALL_UNTRUSTED,
                              flags);

    request->bundle_fd = sd_varlink_take_fd(link, p.fd_index);
    if (request->bundle_fd < 0)
        return sd_varlink_error_invalid_parameter_name(link, "bundleFileDescriptor");
    if (fstat(request->bundle_fd, &st) < 0 || !S_ISREG(st.st_mode))
        return sd_varlink_errorbo(link,
                                  mf_error_to_varlink_id(MF_ERROR_INVALID_BUNDLE),
                                  SD_JSON_BUILD_PAIR_STRING("message", "Not a regular file"));

    /*
     * The bundle is only read once the caller is authorized to install it.
     * TODO: Once there is a store of trusted keys, verify the signature of the
     * index before we ask polkit (reading only the index and its signature),
     * so that a trusted bundle only needs the less strict action.
     */
    return mfd_service_authorize(service, g_steal_pointer(&request), p.flags);
}

/**
 * mfd_varlink_method_install_component:
 */
static int
mfd_varlink_method_install_component(sd_varlink *link,
                                     sd_json_variant *parameters,
                                     sd_varlink_method_flags_t flags,
                                     void *userdata)
{
    MfdService *service = userdata;
    struct {
        const char *component_id;
        const char *origin;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"componentId",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), component_id),
         SD_JSON_MANDATORY},
        {"origin", SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(typeof(p), origin), 0},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    if (!mf_component_id_is_valid(p.component_id))
        return sd_varlink_error_invalid_parameter_name(link, "componentId");
    if (p.origin != NULL && !mf_origin_split_name(p.origin, NULL, NULL))
        return sd_varlink_error_invalid_parameter_name(link, "origin");

    request = mfd_request_new(link,
                              mfd_service_install_component,
                              MF_VARLINK_METHOD_INSTALL_COMPONENT,
                              MFD_POLKIT_ACTION_INSTALL,
                              flags);
    request->repo_request = mfd_repo_request_new_install(p.component_id, p.origin);

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_varlink_method_update:
 */
static int
mfd_varlink_method_update(sd_varlink *link,
                          sd_json_variant *parameters,
                          sd_varlink_method_flags_t flags,
                          void *userdata)
{
    MfdService *service = userdata;
    struct {
        sd_json_variant *component_ids;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"componentIds",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_variant_noref,
         offsetof(typeof(p), component_ids),
         0},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    g_auto(GStrv) component_ids = NULL;
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    if (!mfd_json_variant_get_strv(p.component_ids, &component_ids))
        return sd_varlink_error_invalid_parameter_name(link, "componentIds");
    for (size_t i = 0; component_ids != NULL && component_ids[i] != NULL; i++) {
        if (!mf_component_id_is_valid(component_ids[i]))
            return sd_varlink_error_invalid_parameter_name(link, "componentIds");
    }

    request = mfd_request_new(link,
                              mfd_service_update,
                              MF_VARLINK_METHOD_UPDATE,
                              MFD_POLKIT_ACTION_UPDATE,
                              flags);
    request->repo_request = mfd_repo_request_new_update((const char *const *) component_ids);

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_varlink_method_remove_component:
 */
static int
mfd_varlink_method_remove_component(sd_varlink *link,
                                    sd_json_variant *parameters,
                                    sd_varlink_method_flags_t flags,
                                    void *userdata)
{
    MfdService *service = userdata;
    struct {
        const char *component_id;
        const char *version;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"componentId",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), component_id),
         SD_JSON_MANDATORY},
        {"version", SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(typeof(p), version), 0},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    if (!mf_component_id_is_valid(p.component_id))
        return sd_varlink_error_invalid_parameter_name(link, "componentId");
    if (p.version != NULL && !mf_version_is_valid(p.version))
        return sd_varlink_error_invalid_parameter_name(link, "version");

    request = mfd_request_new(link,
                              mfd_service_remove,
                              MF_VARLINK_METHOD_REMOVE_COMPONENT,
                              MFD_POLKIT_ACTION_REMOVE,
                              flags);
    request->component_id = g_strdup(p.component_id);
    request->version = g_strdup(p.version);

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_varlink_method_cleanup:
 */
static int
mfd_varlink_method_cleanup(sd_varlink *link,
                           sd_json_variant *parameters,
                           sd_varlink_method_flags_t flags,
                           void *userdata)
{
    MfdService *service = userdata;
    static const sd_json_dispatch_field dispatch_table[] = {
        {"flags", _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags, 0, 0},
        {},
    };
    MfTransactionFlags transaction_flags = MF_TRANSACTION_FLAG_NONE;
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &transaction_flags);
    if (r != 0)
        return r;

    /* a request without a component cleans up, see mfd_service_remove() */
    request = mfd_request_new(link,
                              mfd_service_remove,
                              MF_VARLINK_METHOD_CLEANUP,
                              MFD_POLKIT_ACTION_REMOVE,
                              flags);

    return mfd_service_authorize(service, request, transaction_flags);
}

/**
 * mfd_varlink_method_refresh:
 */
static int
mfd_varlink_method_refresh(sd_varlink *link,
                           sd_json_variant *parameters,
                           sd_varlink_method_flags_t flags,
                           void *userdata)
{
    MfdService *service = userdata;
    struct {
        sd_json_variant *remotes;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"remotes",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_variant_noref,
         offsetof(typeof(p), remotes),
         0},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    g_auto(GStrv) remote_ids = NULL;
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    if (!mfd_json_variant_get_strv(p.remotes, &remote_ids))
        return sd_varlink_error_invalid_parameter_name(link, "remotes");

    request = mfd_request_new(link,
                              mfd_service_refresh,
                              MF_VARLINK_METHOD_REFRESH,
                              MFD_POLKIT_ACTION_REFRESH,
                              flags);
    request->remote_ids = g_steal_pointer(&remote_ids);

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_varlink_method_inspect_remote:
 */
static int
mfd_varlink_method_inspect_remote(sd_varlink *link,
                                  sd_json_variant *parameters,
                                  sd_varlink_method_flags_t flags,
                                  void *userdata)
{
    MfdService *service = userdata;
    struct {
        const char *url;
        const char *public_key;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"url",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), url),
         SD_JSON_MANDATORY},
        {"publicKey",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), public_key),
         0},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    g_autofree char *url = NULL;
    g_autofree char *public_key = NULL;
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    url = mf_remote_normalize_url(p.url);
    if (url == NULL)
        return sd_varlink_error_invalid_parameter_name(link, "url");
    if (p.public_key != NULL) {
        public_key = mf_public_key_normalize(p.public_key);
        if (public_key == NULL)
            return sd_varlink_error_invalid_parameter_name(link, "publicKey");
    }

    request = mfd_request_new(link,
                              mfd_service_inspect_remote,
                              MF_VARLINK_METHOD_INSPECT_REMOTE,
                              MFD_POLKIT_ACTION_MANAGE_REMOTES,
                              flags);
    request->url = g_steal_pointer(&url);
    request->public_key = g_steal_pointer(&public_key);

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_varlink_method_add_remote:
 */
static int
mfd_varlink_method_add_remote(sd_varlink *link,
                              sd_json_variant *parameters,
                              sd_varlink_method_flags_t flags,
                              void *userdata)
{
    MfdService *service = userdata;
    struct {
        const char *remote_id;
        const char *url;
        const char *public_key;
        sd_json_variant *channels;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"remoteId",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), remote_id),
         SD_JSON_MANDATORY},
        {"url",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), url),
         SD_JSON_MANDATORY},
        {"publicKey",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), public_key),
         SD_JSON_MANDATORY},
        {"channels",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_variant_noref,
         offsetof(typeof(p), channels),
         0},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    g_autofree char *url = NULL;
    g_autofree char *public_key = NULL;
    g_auto(GStrv) channels = NULL;
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    if (!mf_remote_id_is_valid(p.remote_id))
        return sd_varlink_error_invalid_parameter_name(link, "remoteId");
    url = mf_remote_normalize_url(p.url);
    if (url == NULL)
        return sd_varlink_error_invalid_parameter_name(link, "url");
    public_key = mf_public_key_normalize(p.public_key);
    if (public_key == NULL)
        return sd_varlink_error_invalid_parameter_name(link, "publicKey");
    if (!mfd_json_variant_get_strv(p.channels, &channels))
        return sd_varlink_error_invalid_parameter_name(link, "channels");
    for (size_t i = 0; channels != NULL && channels[i] != NULL; i++) {
        if (!mf_remote_channel_is_valid(channels[i]) ||
            g_strv_contains((const char *const *) channels + i + 1, channels[i]))
            return sd_varlink_error_invalid_parameter_name(link, "channels");
    }

    request = mfd_request_new(link,
                              mfd_service_add_remote,
                              MF_VARLINK_METHOD_ADD_REMOTE,
                              MFD_POLKIT_ACTION_MANAGE_REMOTES,
                              flags);
    request->remote_id = g_strdup(p.remote_id);
    request->url = g_steal_pointer(&url);
    request->public_key = g_steal_pointer(&public_key);
    request->channels = g_steal_pointer(&channels);

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_varlink_method_remove_remote:
 */
static int
mfd_varlink_method_remove_remote(sd_varlink *link,
                                 sd_json_variant *parameters,
                                 sd_varlink_method_flags_t flags,
                                 void *userdata)
{
    MfdService *service = userdata;
    struct {
        const char *remote_id;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"remoteId",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), remote_id),
         SD_JSON_MANDATORY},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    if (!mf_remote_id_is_valid(p.remote_id))
        return sd_varlink_error_invalid_parameter_name(link, "remoteId");

    request = mfd_request_new(link,
                              mfd_service_remove_remote,
                              MF_VARLINK_METHOD_REMOVE_REMOTE,
                              MFD_POLKIT_ACTION_MANAGE_REMOTES,
                              flags);
    request->remote_id = g_strdup(p.remote_id);

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_varlink_method_mark_component:
 */
static int
mfd_varlink_method_mark_component(sd_varlink *link,
                                  sd_json_variant *parameters,
                                  sd_varlink_method_flags_t flags,
                                  void *userdata)
{
    MfdService *service = userdata;
    struct {
        const char *component_id;
        const char *version;
        const char *mode;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"componentId",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), component_id),
         SD_JSON_MANDATORY},
        {"version", SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(typeof(p), version), 0},
        {"mode",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), mode),
         SD_JSON_MANDATORY},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    MfdRequest *request;
    MfInstallMode mode;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    if (!mf_component_id_is_valid(p.component_id))
        return sd_varlink_error_invalid_parameter_name(link, "componentId");
    if (p.version != NULL && !mf_version_is_valid(p.version))
        return sd_varlink_error_invalid_parameter_name(link, "version");
    mode = mf_install_mode_from_string(p.mode);
    if (mode == MF_INSTALL_MODE_UNKNOWN)
        return sd_varlink_error_invalid_parameter_name(link, "mode");

    request = mfd_request_new(link,
                              mfd_service_mark,
                              MF_VARLINK_METHOD_MARK_COMPONENT,
                              MFD_POLKIT_ACTION_MARK,
                              flags);
    request->component_id = g_strdup(p.component_id);
    request->version = g_strdup(p.version);
    request->mode = mode;

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_service_new:
 * @event: The event loop to run the service in.
 * @error: A #GError
 *
 * Create the service. It does not listen for connections yet.
 */
MfdService *
mfd_service_new(sd_event *event, GError **error)
{
    g_autoptr(MfdService) service = NULL;
    int r;

    service = g_object_new(MFD_TYPE_SERVICE, NULL);
    service->manager = mfd_manager_new();

    service->server = mf_varlink_server_new(event,
                                            "manifoldd",
#if HAVE_SD_VARLINK_SERVER_FD_PASSING
                                            SD_VARLINK_SERVER_ALLOW_FD_PASSING_INPUT,
#else
                                            0, /* mfd_service_on_connect() allows fd passing per connection */
#endif
                                            &vl_interface_org_freedesktop_manifold,
                                            service,
                                            error);
    if (service->server == NULL)
        return NULL;

    r = sd_varlink_server_bind_method_many(service->server,
                                           MF_VARLINK_METHOD_INSTALL_FILE,
                                           mfd_varlink_method_install_file,
                                           MF_VARLINK_METHOD_INSTALL_COMPONENT,
                                           mfd_varlink_method_install_component,
                                           MF_VARLINK_METHOD_UPDATE,
                                           mfd_varlink_method_update,
                                           MF_VARLINK_METHOD_REMOVE_COMPONENT,
                                           mfd_varlink_method_remove_component,
                                           MF_VARLINK_METHOD_CLEANUP,
                                           mfd_varlink_method_cleanup,
                                           MF_VARLINK_METHOD_MARK_COMPONENT,
                                           mfd_varlink_method_mark_component,
                                           MF_VARLINK_METHOD_REFRESH,
                                           mfd_varlink_method_refresh,
                                           MF_VARLINK_METHOD_INSPECT_REMOTE,
                                           mfd_varlink_method_inspect_remote,
                                           MF_VARLINK_METHOD_ADD_REMOTE,
                                           mfd_varlink_method_add_remote,
                                           MF_VARLINK_METHOD_REMOVE_REMOTE,
                                           mfd_varlink_method_remove_remote);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to bind Varlink methods");
        return NULL;
    }

    /* the queue runner is only enabled while there are requests in the queue */
    r = sd_event_add_defer(event, &service->queue_runner, mfd_service_on_run_queue, service);
    if (r >= 0)
        r = sd_event_source_set_enabled(service->queue_runner, SD_EVENT_OFF);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to set up the request queue");
        return NULL;
    }

    return g_steal_pointer(&service);
}

/**
 * mfd_service_on_idle_timeout:
 */
static int
mfd_service_on_idle_timeout(sd_event_source *source, uint64_t usec, void *userdata)
{
    g_info("Nothing to do, exiting");
    return sd_event_exit(sd_event_source_get_event(source), 0);
}

/**
 * mfd_service_on_connect:
 */
static int
mfd_service_on_connect(sd_varlink_server *server, sd_varlink *link, void *userdata)
{
    MfdService *service = userdata;

#if !HAVE_SD_VARLINK_SERVER_FD_PASSING
    /* systemd < 258 can only allow receiving file descriptors per connection,
     * and we need to receive the file descriptors of the bundles that clients ask us to install */
    int r = sd_varlink_set_allow_fd_passing_input(link, true);
    if (r < 0)
        return r;
#endif

    /* the idle timer only exists when we were socket activated */
    if (service->idle_timer != NULL)
        sd_event_source_set_enabled(service->idle_timer, SD_EVENT_OFF);
    return 0;
}

/**
 * mfd_service_on_disconnect:
 */
static void
mfd_service_on_disconnect(sd_varlink_server *server, sd_varlink *link, void *userdata)
{
    MfdService *service = userdata;
    MfdRequest *request = g_hash_table_lookup(service->requests, link);

    /* drop the request of the disconnected caller */
    if (request != NULL) {
        g_message("User %u disconnected before their call of %s was done",
                  (guint) request->uid,
                  request->method);
        mfd_service_drop_request(service, request);
    }

    /* the idle timer only exists when we were socket activated */
    if (service->idle_timer == NULL)
        return;
    /* the closing connection is not included in the count anymore */
    if (sd_varlink_server_current_connections(server) > 0)
        return;
    sd_event_source_set_time_relative(service->idle_timer, IDLE_TIMEOUT_USEC);
    sd_event_source_set_enabled(service->idle_timer, SD_EVENT_ONESHOT);
}

/**
 * mfd_service_exit_when_idle:
 *
 * Exit once no connection was open for a while.
 *
 * We do not use the exit-on-idle feature of sd-varlink, as it exits the event
 * loop as soon as no connection is open. If we were started with a listening
 * socket, no connection is open yet when the event loop starts, so we would
 * exit immediately.
 */
static int
mfd_service_exit_when_idle(MfdService *service)
{
    /* the timer is stopped while a connection is open, see mfd_service_on_connect()
     * and mfd_service_on_disconnect() */
    return sd_event_add_time_relative(sd_varlink_server_get_event(service->server),
                                      &service->idle_timer,
                                      CLOCK_MONOTONIC,
                                      IDLE_TIMEOUT_USEC,
                                      G_USEC_PER_SEC,
                                      mfd_service_on_idle_timeout,
                                      NULL);
}

/**
 * mfd_service_set_fetch_helper:
 *
 * Make the service run the download helper itself. This is used by tests.
 */
void
mfd_service_set_fetch_helper(MfdService *service, const char *helper_path, const char *ca_file)
{
    mfd_manager_set_fetch_helper(service->manager, helper_path, ca_file);
}

/**
 * mfd_service_listen:
 * @service: The service.
 * @socket_path: (nullable): Path of the socket to listen on, or %NULL
 * @error: A #GError
 *
 * Listen for connections. If no socket path is given, we listen on the socket
 * we were activated with, and exit once we are idle.
 */
gboolean
mfd_service_listen(MfdService *service, const char *socket_path, GError **error)
{
    gboolean activated;
    int r;

    sd_varlink_server_bind_connect(service->server, mfd_service_on_connect);
    sd_varlink_server_bind_disconnect(service->server, mfd_service_on_disconnect);
    if (!mf_varlink_server_listen(service->server, socket_path, MF_SOCKET_PATH, &activated, error))
        return FALSE;
    if (!activated)
        return TRUE;

    r = mfd_service_exit_when_idle(service);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to set up the idle timer");
        return FALSE;
    }

    return TRUE;
}
