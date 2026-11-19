/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include "config.h"
#include "mfd-fetcher.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>
#include <libglnx.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include <manifold.h>
#include "mf-fetch-interface.h"
#include "mf-sdautoptr-private.h"

/* the file descriptor number on which a spawned helper serves its connection */
constexpr int HELPER_CONNECTION_FD = 3;

/**
 * MfdFetcher:
 *
 * A connection to the download helper (see manifold-fetch.c). systemd starts
 * a sandboxed helper when we connect to its socket. For tests, we can run
 * the helper ourselves instead.
 *
 * A fetcher is shared by all code that downloads during one operation, so it
 * is reference counted. It connects to the helper when the first download
 * starts.
 */
struct _MfdFetcher {
    gatomicrefcount ref_count;
    char *helper_path; /* the helper to run ourselves, or NULL to connect to the systemd socket */
    char *ca_file;     /* the only certificate authority that a spawned helper trusts, or NULL */
    sd_varlink *link;  /* the connection to the helper, or NULL if we did not connect yet */
    GPid helper_pid;   /* the PID of the spawned helper, or 0 */
};

/**
 * mfd_fetch_request_new:
 * @fd: the file to write to, owned by the caller
 *
 * Returns: (transfer full): a request to download a whole file
 */
MfdFetchRequest *
mfd_fetch_request_new(const char *url, int fd, guint64 max_size)
{
    MfdFetchRequest *request = g_new0(MfdFetchRequest, 1);

    request->url = g_strdup(url);
    request->fd = fd;
    request->max_size = max_size;
    return request;
}

/**
 * mfd_fetch_request_free:
 */
void
mfd_fetch_request_free(MfdFetchRequest *request)
{
    g_free(request->url);
    g_clear_pointer(&request->ranges, g_array_unref);
    g_free(request->failure);
    g_free(request->message);
    g_free(request);
}

/**
 * mfd_fetch_request_to_error:
 *
 * Set an error that describes why a request failed.
 */
void
mfd_fetch_request_to_error(MfdFetchRequest *request, GError **error)
{
    if (request->http_status > 0)
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_DOWNLOAD_FAILED,
                    /* TRANSLATORS: the URL of a file, and the HTTP status the server replied with */
                    _("Unable to download %s: the server replied with %" G_GINT64_FORMAT),
                    request->url,
                    request->http_status);
    else
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_DOWNLOAD_FAILED,
                    /* TRANSLATORS: the URL of a file, and the error message */
                    _("Unable to download %s: %s"),
                    request->url,
                    request->message != NULL ? request->message : request->failure);
}

/**
 * mfd_fetcher_ref:
 *
 * Returns: (transfer full): @fetcher
 */
MfdFetcher *
mfd_fetcher_ref(MfdFetcher *fetcher)
{
    g_atomic_ref_count_inc(&fetcher->ref_count);
    return fetcher;
}

/**
 * mfd_fetcher_unref:
 *
 * Drop a reference. When the last reference is dropped, the connection is
 * closed, which makes the helper exit.
 */
void
mfd_fetcher_unref(MfdFetcher *fetcher)
{
    if (fetcher == NULL || !g_atomic_ref_count_dec(&fetcher->ref_count))
        return;
    g_clear_pointer(&fetcher->link, sd_varlink_flush_close_unref);
    if (fetcher->helper_pid > 0) {
        while (waitpid(fetcher->helper_pid, NULL, 0) < 0 && errno == EINTR)
            ;
        g_spawn_close_pid(fetcher->helper_pid);
    }
    g_free(fetcher->helper_path);
    g_free(fetcher->ca_file);
    g_free(fetcher);
}

/**
 * mfd_fetcher_spawn_helper:
 *
 * Spawn the helper ourselves, and connect it to one end of a socket pair.
 *
 * Returns: the file descriptor of our end of the socket pair, or -1 on error
 */
static int
mfd_fetcher_spawn_helper(MfdFetcher *fetcher, GError **error)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    g_autofree char *fd_arg = g_strdup_printf("%d", HELPER_CONNECTION_FD);
    int target_fds[] = {HELPER_CONNECTION_FD};
    int pair[2];
    g_autofd int our_fd = -1;
    g_autofd int helper_fd = -1;
    gboolean ret;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "Unable to create a socket pair: %s",
                    g_strerror(errno));
        return -1;
    }
    our_fd = pair[0];
    helper_fd = pair[1];

    g_ptr_array_add(argv, fetcher->helper_path);
    g_ptr_array_add(argv, (gpointer) "--connection-fd");
    g_ptr_array_add(argv, fd_arg);
    if (fetcher->ca_file != NULL) {
        g_ptr_array_add(argv, (gpointer) "--ca-file");
        g_ptr_array_add(argv, fetcher->ca_file);
    }
    g_ptr_array_add(argv, NULL);

    ret = g_spawn_async_with_pipes_and_fds(NULL,
                                           (const char *const *) argv->pdata,
                                           NULL,
                                           G_SPAWN_DO_NOT_REAP_CHILD,
                                           NULL,
                                           NULL,
                                           -1,
                                           -1,
                                           -1,
                                           &helper_fd,
                                           target_fds,
                                           1,
                                           &fetcher->helper_pid,
                                           NULL,
                                           NULL,
                                           NULL,
                                           error);
    if (!ret)
        return -1;
    return g_steal_fd(&our_fd);
}

/**
 * mfd_fetcher_new:
 * @helper_path: (nullable): the helper to run ourselves, or %NULL to use the systemd service
 * @ca_file: (nullable): the only certificate authority that a spawned helper should trust
 *
 * Returns: (transfer full): a new #MfdFetcher
 */
MfdFetcher *
mfd_fetcher_new(const char *helper_path, const char *ca_file)
{
    MfdFetcher *fetcher = g_new0(MfdFetcher, 1);

    g_atomic_ref_count_init(&fetcher->ref_count);
    fetcher->helper_path = g_strdup(helper_path);
    fetcher->ca_file = g_strdup(ca_file);
    return fetcher;
}

/**
 * mfd_fetcher_connect:
 *
 * Connect to the download helper, unless we are connected already.
 */
static gboolean
mfd_fetcher_connect(MfdFetcher *fetcher, GError **error)
{
    int r;

    if (fetcher->link != NULL)
        return TRUE;

    if (fetcher->helper_path == NULL) {
        r = sd_varlink_connect_address(&fetcher->link, MF_FETCH_SOCKET_PATH);
    } else {
        g_autofd int fd = mfd_fetcher_spawn_helper(fetcher, error);

        if (fd < 0)
            return FALSE;
        /* the connection only takes ownership of the file descriptor if it was set up */
        r = sd_varlink_connect_fd(&fetcher->link, fd);
        if (r >= 0)
            g_steal_fd(&fd);
    }
    if (r < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Unable to reach the download helper: %s"),
                    g_strerror(-r));
        return FALSE;
    }

    /* a download may take a long time, so the call has no timeout. The helper gives up
     * on stalled transfers itself. */
    if (sd_varlink_set_allow_fd_passing_output(fetcher->link, true) < 0 ||
        sd_varlink_set_relative_timeout(fetcher->link, UINT64_MAX) < 0) {
        g_clear_pointer(&fetcher->link, sd_varlink_close_unref);
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_FAILED,
                            "Unable to set up the connection to the download helper");
        return FALSE;
    }
    sd_varlink_set_description(fetcher->link, "manifold-fetch");

    return TRUE;
}

/* one call to the helper, with a batch of requests */
typedef struct {
    GPtrArray *requests;     /* (element-type MfdFetchRequest) */
    guint first;             /* the index of the first request of the batch */
    guint n_requests;        /* the number of requests in the batch */
    guint64 received_before; /* the number of bytes received by previous calls */
    MfdFetchProgressFunc progress_func;
    gpointer user_data;
    gboolean done; /* whether the final reply has arrived */
    GError *error;
} MfdFetchCall;

/**
 * mfd_fetch_call_read_results:
 *
 * Read the result of each request of a call from the final reply.
 *
 * Returns: %FALSE if the reply does not have a result for each request
 */
static gboolean
mfd_fetch_call_read_results(MfdFetchCall *call, sd_json_variant *results)
{
    if (results == NULL || !sd_json_variant_is_array(results) ||
        sd_json_variant_elements(results) != call->n_requests)
        return FALSE;

    for (guint i = 0; i < call->n_requests; i++) {
        MfdFetchRequest *request = g_ptr_array_index(call->requests, call->first + i);
        sd_json_variant *result = sd_json_variant_by_index(results, i);
        sd_json_variant *value;

        if (!sd_json_variant_is_object(result))
            return FALSE;
        value = sd_json_variant_by_key(result, "size");
        request->size = value != NULL ? sd_json_variant_unsigned(value) : 0;
        value = sd_json_variant_by_key(result, "failure");
        if (value != NULL && sd_json_variant_is_string(value))
            request->failure = g_strdup(sd_json_variant_string(value));
        value = sd_json_variant_by_key(result, "httpStatus");
        if (value != NULL && sd_json_variant_is_integer(value))
            request->http_status = sd_json_variant_integer(value);
        value = sd_json_variant_by_key(result, "message");
        if (value != NULL && sd_json_variant_is_string(value))
            request->message = g_strdup(sd_json_variant_string(value));
    }

    return TRUE;
}

/**
 * mfd_on_fetch_reply:
 *
 * Handle a reply of the helper: forward the progress, and read the results
 * from the final reply.
 */
static int
mfd_on_fetch_reply(sd_varlink *link,
                   sd_json_variant *parameters,
                   const char *error_id,
                   sd_varlink_reply_flags_t flags,
                   void *userdata)
{
    MfdFetchCall *call = userdata;
    sd_json_variant *received = sd_json_variant_by_key(parameters, "received");

    if (error_id != NULL) {
        g_set_error(&call->error, MF_ERROR, MF_ERROR_FAILED, _("The download helper failed: %s"), error_id);
        call->done = TRUE;
        return 0;
    }
    if (call->progress_func != NULL && received != NULL && sd_json_variant_is_unsigned(received))
        call->progress_func(call->received_before + sd_json_variant_unsigned(received), call->user_data);
    if ((flags & SD_VARLINK_REPLY_CONTINUES) != 0)
        return 0;

    if (!mfd_fetch_call_read_results(call, sd_json_variant_by_key(parameters, "results")))
        g_set_error_literal(&call->error,
                            MF_ERROR,
                            MF_ERROR_FAILED,
                            _("The reply of the download helper does not match the request"));
    call->done = TRUE;
    return 0;
}

/**
 * mfd_fetcher_call:
 *
 * Send one call to the helper, and wait for its final reply.
 */
static gboolean
mfd_fetcher_call(MfdFetcher *fetcher, MfdFetchCall *call, GError **error)
{
    g_autoptr(sd_json_variant) list = NULL;
    int r;

    for (guint i = 0; i < call->n_requests; i++) {
        MfdFetchRequest *request = g_ptr_array_index(call->requests, call->first + i);
        g_autoptr(sd_json_variant) ranges = NULL;
        int index;

        for (guint j = 0; request->ranges != NULL && j < request->ranges->len; j++) {
            MfdFetchRange *range = &g_array_index(request->ranges, MfdFetchRange, j);

            r = sd_json_variant_append_arraybo(&ranges,
                                               SD_JSON_BUILD_PAIR_UNSIGNED("offset", range->offset),
                                               SD_JSON_BUILD_PAIR_UNSIGNED("length", range->length));
            if (r < 0)
                goto fail;
        }

        /* the helper gets a duplicate of the file descriptor, so our copy stays open */
        index = sd_varlink_push_dup_fd(fetcher->link, request->fd);
        if (index < 0) {
            r = index;
            goto fail;
        }
        r = sd_json_variant_append_arraybo(
            &list,
            SD_JSON_BUILD_PAIR_STRING("url", request->url),
            SD_JSON_BUILD_PAIR_UNSIGNED("fileDescriptor", index),
            SD_JSON_BUILD_PAIR_UNSIGNED("maxSize", request->max_size),
            SD_JSON_BUILD_PAIR_CONDITION(ranges != NULL, "ranges", SD_JSON_BUILD_VARIANT(ranges)));
        if (r < 0)
            goto fail;
    }

    sd_varlink_set_userdata(fetcher->link, call);
    sd_varlink_bind_reply(fetcher->link, mfd_on_fetch_reply);
    r = sd_varlink_observebo(fetcher->link,
                             MF_FETCH_METHOD_FETCH,
                             SD_JSON_BUILD_PAIR_VARIANT("requests", list));
    if (r < 0)
        goto fail;

    /* manifoldd handles one request at a time, so we can block here until the helper is done */
    while (!call->done) {
        r = sd_varlink_process(fetcher->link);
        if (r < 0)
            goto fail;
        if (r > 0)
            continue;
        r = sd_varlink_wait(fetcher->link, UINT64_MAX);
        if (r < 0)
            goto fail;
    }
    sd_varlink_set_userdata(fetcher->link, NULL);

    if (call->error != NULL) {
        g_propagate_error(error, g_steal_pointer(&call->error));
        return FALSE;
    }
    return TRUE;

fail:
    sd_varlink_set_userdata(fetcher->link, NULL);
    g_clear_error(&call->error);
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_FAILED,
                _("Unable to talk to the download helper: %s"),
                g_strerror(-r));
    return FALSE;
}

/**
 * mfd_fetcher_fetch:
 * @requests: (element-type MfdFetchRequest): the files or ranges to download
 * @progress_func: (nullable): called with the number of bytes received so far
 *
 * Download files or ranges of files into the file descriptors of the requests.
 * A request that fails records the reason for its failure, and does not stop
 * the other requests.
 *
 * Returns: %FALSE if the helper could not be called
 */
gboolean
mfd_fetcher_fetch(MfdFetcher *fetcher,
                  GPtrArray *requests,
                  MfdFetchProgressFunc progress_func,
                  gpointer user_data,
                  GError **error)
{
    guint64 received = 0;

    if (!mfd_fetcher_connect(fetcher, error))
        return FALSE;

    for (guint first = 0; first < requests->len; first += MF_FETCH_REQUESTS_MAX) {
        MfdFetchCall call = {
            .requests = requests,
            .first = first,
            .n_requests = MIN(MF_FETCH_REQUESTS_MAX, requests->len - first),
            .received_before = received,
            .progress_func = progress_func,
            .user_data = user_data,
        };

        if (!mfd_fetcher_call(fetcher, &call, error))
            return FALSE;
        for (guint i = 0; i < call.n_requests; i++)
            received += ((MfdFetchRequest *) g_ptr_array_index(requests, first + i))->size;
    }

    return TRUE;
}

/**
 * mfd_read_fd:
 * @max_size: the largest the file may be
 *
 * Read a file that the download helper wrote. The helper handles untrusted
 * data from the network, so we check the size limit again ourselves.
 *
 * Returns: (transfer full): the contents of @fd, or %NULL
 */
static GBytes *
mfd_read_fd(int fd, guint64 max_size, GError **error)
{
    g_autofree guint8 *data = NULL;
    struct stat st;
    gsize done = 0;

    if (fstat(fd, &st) < 0)
        return glnx_null_throw_errno_prefix(error, "Unable to read a download");
    if ((guint64) st.st_size > max_size) {
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_DOWNLOAD_FAILED,
                            _("The download helper wrote more data than it was allowed to"));
        return NULL;
    }
    data = g_malloc(st.st_size > 0 ? (gsize) st.st_size : 1);
    while (done < (gsize) st.st_size) {
        ssize_t n = pread(fd, data + done, (gsize) st.st_size - done, (off_t) done);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return glnx_null_throw_errno_prefix(error, "Unable to read a download");
        done += (gsize) n;
    }

    return g_bytes_new_take(g_steal_pointer(&data), done);
}

/**
 * mfd_memory_download_clear:
 */
void
mfd_memory_download_clear(MfdMemoryDownload *download)
{
    g_clear_pointer(&download->url, g_free);
    g_clear_pointer(&download->data, g_bytes_unref);
}

/**
 * mfd_fetcher_download_to_memory:
 * @downloads: (array length=n_downloads): the files to download
 *
 * Download small files into memory, all at once. The content of each file
 * is stored in its #MfdMemoryDownload.
 *
 * Returns: %TRUE if every file was downloaded
 */
gboolean
mfd_fetcher_download_to_memory(MfdFetcher *fetcher,
                               MfdMemoryDownload **downloads,
                               guint n_downloads,
                               GError **error)
{
    g_autoptr(GPtrArray) requests = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_fetch_request_free);
    gboolean ret = FALSE;

    for (guint i = 0; i < n_downloads; i++) {
        int fd = memfd_create("manifold-download", MFD_CLOEXEC);

        if (fd < 0) {
            glnx_throw_errno_prefix(error, "Unable to create a file to download to");
            goto out;
        }
        g_ptr_array_add(requests, mfd_fetch_request_new(downloads[i]->url, fd, downloads[i]->max_size));
    }

    if (!mfd_fetcher_fetch(fetcher, requests, NULL, NULL, error))
        goto out;
    for (guint i = 0; i < n_downloads; i++) {
        MfdFetchRequest *request = g_ptr_array_index(requests, i);

        if (request->failure != NULL) {
            mfd_fetch_request_to_error(request, error);
            goto out;
        }
        g_clear_pointer(&downloads[i]->data, g_bytes_unref);
        downloads[i]->data = mfd_read_fd(request->fd, downloads[i]->max_size, error);
        if (downloads[i]->data == NULL)
            goto out;
    }
    ret = TRUE;

out:
    /* a request does not own its file descriptor, so we close the descriptors here */
    for (guint i = 0; i < requests->len; i++)
        close(((MfdFetchRequest *) g_ptr_array_index(requests, i))->fd);
    return ret;
}

/**
 * mfd_fetcher_download_file:
 * @max_size: the largest the file may be
 *
 * Download one small file into memory.
 *
 * Returns: (transfer full): the content of the file, or %NULL on error
 */
GBytes *
mfd_fetcher_download_file(MfdFetcher *fetcher, const char *url, guint64 max_size, GError **error)
{
    g_auto(MfdMemoryDownload) download = {.url = g_strdup(url), .max_size = max_size};
    MfdMemoryDownload *downloads[] = {&download};

    if (!mfd_fetcher_download_to_memory(fetcher, downloads, G_N_ELEMENTS(downloads), error))
        return NULL;
    return g_steal_pointer(&download.data);
}
