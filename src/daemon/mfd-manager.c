/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-manager.h"

#include <sys/stat.h>
#include <appstream.h>

#include "mf-component-private.h"
#include "mf-result-private.h"
#include "mf-store-private.h"
#include "mf-progress-private.h"
#include "mf-utils-private.h"

#include "mfd-image-reader.h"
#include "mfd-install-set.h"
#include "mfd-integrator.h"
#include "mfd-refresh.h"
#include "mfd-store-writer.h"
#include "mfd-utils.h"

/**
 * MfdManager:
 *
 * Installs, updates and removes components in the local store.
 *
 * The manager plans every operation, and checks that it can be done before
 * anything is written. The #MfdStoreWriter then makes the changes to the
 * store. Progress is reported through a #MfProgressFunc, and the changes
 * that were made are returned as #MfResult.
 *
 * Any permission checks happen in the caller.
 */
struct _MfdManager {
    GObject parent_instance;

    MfStore *store;
    MfdStoreWriter *writer;
    MfdIntegrator *integrator;

    /* path of a download helper that we start ourselves, or NULL to use the systemd service */
    char *fetch_helper;
    /* the only certificate authority file that a self-started download helper trusts, or NULL */
    char *fetch_ca_file;
};

G_DEFINE_TYPE(MfdManager, mfd_manager, G_TYPE_OBJECT)

static void
mfd_manager_finalize(GObject *object)
{
    MfdManager *manager = MFD_MANAGER(object);

    g_clear_object(&manager->integrator);
    g_clear_object(&manager->writer);
    g_clear_object(&manager->store);
    g_free(manager->fetch_helper);
    g_free(manager->fetch_ca_file);

    G_OBJECT_CLASS(mfd_manager_parent_class)->finalize(object);
}

static void
mfd_manager_class_init(MfdManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mfd_manager_finalize;
}

static void
mfd_manager_init(MfdManager *manager)
{
}

/**
 * mfd_manager_new_for_store:
 * @store: the store to manage
 *
 * Returns: (transfer full): a new #MfdManager
 */
MfdManager *
mfd_manager_new_for_store(MfStore *store)
{
    MfdManager *manager = g_object_new(MFD_TYPE_MANAGER, NULL);

    manager->store = g_object_ref(store);
    manager->writer = mfd_store_writer_new(store);
    manager->integrator = mfd_integrator_new(store, manager->writer);
    return manager;
}

/**
 * mfd_manager_new:
 *
 * Returns: (transfer full): a new #MfdManager for the store of the system
 */
MfdManager *
mfd_manager_new(void)
{
    g_autoptr(MfStore) store = mf_store_new();

    return mfd_manager_new_for_store(store);
}

/**
 * mfd_error_detail_clear:
 */
void
mfd_error_detail_clear(MfdErrorDetail *detail)
{
    g_clear_pointer(&detail->component_id, g_free);
    g_clear_pointer(&detail->version, g_free);
    g_clear_pointer(&detail->base_id, g_free);
    g_clear_pointer(&detail->base_version, g_free);
    g_clear_pointer(&detail->identity, g_free);
    g_clear_pointer(&detail->used_by, g_free);
    g_clear_pointer(&detail->architecture, g_free);
    detail->kind = MF_COMPONENT_KIND_UNKNOWN;
    detail->sdk = FALSE;
}

/**
 * MfdProgressTracker:
 *
 * Progress state of the current operation: the #MfProgress that is
 * reported, and the counters that it is calculated from.
 */
typedef struct {
    MfProgressFunc func;
    gpointer user_data;
    MfProgress *progress;
    MfItemProgress *item; /* the item being worked on, owned by @progress */
    guint total;          /* number of items of the operation */
    guint done;           /* number of finished items */
    guint64 bytes_last_reported;
    gboolean item_done;
} MfdProgressTracker;

/**
 * mfd_progress_tracker_clear:
 */
static void
mfd_progress_tracker_clear(MfdProgressTracker *tracker)
{
    g_clear_pointer(&tracker->progress, mf_progress_free);
    tracker->item = NULL;
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfdProgressTracker, mfd_progress_tracker_clear)

/* the number of bytes of an image that we copy between two progress reports */
constexpr guint64 PROGRESS_STEP_BYTES = 4 * 1024 * 1024;

/**
 * mfd_progress_report:
 *
 * Report the current progress of the operation to the caller.
 */
static void
mfd_progress_report(MfdProgressTracker *tracker)
{
    double item_percent = 0;

    if (tracker->func == NULL)
        return;

    if (tracker->item != NULL) {
        guint64 bytes_done = mf_item_progress_get_bytes_done(tracker->item);
        guint64 bytes_total = mf_item_progress_get_bytes_total(tracker->item);

        if (bytes_total > 0)
            item_percent = (double) bytes_done * 100 / (double) bytes_total;
        else if (tracker->item_done)
            item_percent = 100;
        mf_item_progress_set_percent(tracker->item, item_percent);
        tracker->bytes_last_reported = bytes_done;
    }
    double percent = tracker->total > 0
                         ? ((double) tracker->done * 100 + item_percent) / (double) tracker->total
                         : 100;
    mf_progress_set_percent(tracker->progress, MIN(percent, 100));

    tracker->func(tracker->progress, tracker->user_data);
}

/**
 * mfd_progress_begin_item:
 * @version: (nullable): the version of the component that the item is about
 * @bytes_total: the size of the image the item copies, or 0
 *
 * Report that work on an item begins.
 */
static void
mfd_progress_begin_item(MfdProgressTracker *tracker,
                        MfAction action,
                        const char *subject,
                        const char *version,
                        guint64 bytes_total)
{
    tracker->item = mf_item_progress_new(action, subject, version);
    mf_item_progress_set_bytes_total(tracker->item, bytes_total);
    mf_progress_take_item(tracker->progress, tracker->item);
    tracker->bytes_last_reported = 0;
    tracker->item_done = FALSE;
    mfd_progress_report(tracker);
}

/**
 * mfd_progress_end_item:
 *
 * Report that the item is done.
 */
static void
mfd_progress_end_item(MfdProgressTracker *tracker)
{
    mf_item_progress_set_bytes_done(tracker->item, mf_item_progress_get_bytes_total(tracker->item));
    tracker->item_done = TRUE;
    mfd_progress_report(tracker);
    mf_progress_take_item(tracker->progress, NULL);
    tracker->item = NULL;
    tracker->done++;
}

/**
 * mfd_progress_split_last_item:
 * @n_items: the number of items to split the last item into
 *
 * Split the last item of the operation into @n_items items, which together
 * take the share of the progress that the last item had. If @n_items is zero,
 * the last item is counted as done right away.
 */
static void
mfd_progress_split_last_item(MfdProgressTracker *tracker, guint n_items)
{
    if (n_items == 0) {
        tracker->done = tracker->total;
        mfd_progress_report(tracker);
        return;
    }
    tracker->total *= n_items;
    tracker->done *= n_items;
}

/**
 * mfd_on_copy_progress:
 *
 * Report the progress of writing an image into the store.
 */
static void
mfd_on_copy_progress(guint64 bytes_copied, gpointer user_data)
{
    MfdProgressTracker *tracker = user_data;

    mf_item_progress_set_bytes_done(tracker->item, bytes_copied);
    /* the last bytes are reported when the item ends */
    if (bytes_copied - tracker->bytes_last_reported < PROGRESS_STEP_BYTES ||
        bytes_copied == mf_item_progress_get_bytes_total(tracker->item))
        return;
    mfd_progress_report(tracker);
}

/* the arguments of mfd_write_item_image() */
typedef struct {
    MfdInstallItem *item;
    MfdProgressTracker *tracker;
} MfdItemImageWrite;

/**
 * mfd_write_item_image:
 *
 * Write the image of an install item to a file of the store.
 */
static gboolean
mfd_write_item_image(int fd, gpointer user_data, GError **error)
{
    MfdItemImageWrite *write = user_data;

    return write->item->write_image(write->item, fd, mfd_on_copy_progress, write->tracker, error);
}

/**
 * MfdInstallPlan:
 *
 * The result of checking one item of an install set, which says how the item will be installed.
 */
typedef struct {
    MfdInstallItem *item; /* the item of the set, which outlives the plan */
    MfInstallMode mode;
    gboolean skip;      /* the item is installed already or is not needed, so there is nothing to do */
    gboolean add_image; /* the image has to be added to the store */
    MfBaseLink *base;   /* link to the base of the component, NULL for a core */
} MfdInstallPlan;

/**
 * mfd_install_plan_clear:
 */
static void
mfd_install_plan_clear(MfdInstallPlan *iplan)
{
    g_clear_pointer(&iplan->base, mf_base_link_free);
}

/**
 * mfd_install_plan_compare_layer:
 *
 * Order plans by the layer of their component, from the bottom to the top,
 * so every component comes after the components that it builds on.
 */
static gint
mfd_install_plan_compare_layer(gconstpointer a, gconstpointer b)
{
    MfManifest *manifest_a = ((const MfdInstallPlan *) a)->item->manifest;
    MfManifest *manifest_b = ((const MfdInstallPlan *) b)->item->manifest;
    MfComponentKind kind_a = mf_manifest_get_kind(manifest_a);
    MfComponentKind kind_b = mf_manifest_get_kind(manifest_b);

    if (kind_a != kind_b)
        return kind_a < kind_b ? -1 : 1;
    /* an SDK builds on a component of its own kind */
    return (int) mf_manifest_is_sdk(manifest_a) - (int) mf_manifest_is_sdk(manifest_b);
}

/**
 * mfd_set_error_already_installed:
 */
static void
mfd_set_error_already_installed(GError **error,
                                MfdErrorDetail *ret_detail,
                                const char *component_id,
                                const char *version)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->version = g_strdup(version);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_ALREADY_INSTALLED,
                "%s %s is installed already",
                component_id,
                version);
}

/**
 * mfd_set_error_not_installed:
 * @version: (nullable): the version, or %NULL for any version of the component
 */
static void
mfd_set_error_not_installed(GError **error,
                            MfdErrorDetail *ret_detail,
                            const char *component_id,
                            const char *version)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->version = g_strdup(version);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_NOT_FOUND,
                "%s%s%s is not installed",
                component_id,
                version != NULL ? " " : "",
                version != NULL ? version : "");
}

/**
 * mfd_set_error_in_use:
 * @used_by: the ID of a component that is kept, and that builds on @component_id
 */
static void
mfd_set_error_in_use(GError **error,
                     MfdErrorDetail *ret_detail,
                     const char *component_id,
                     const char *used_by)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->used_by = g_strdup(used_by);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_COMPONENT_IN_USE,
                "Component %s is used by %s",
                component_id,
                used_by);
}

/**
 * mfd_set_error_missing_base:
 * @manifest: the manifest of the component whose base is missing
 * @base_version: the version of the base, with its revision if only that exact revision will do
 */
static void
mfd_set_error_missing_base(GError **error,
                           MfdErrorDetail *ret_detail,
                           MfManifest *manifest,
                           const char *base_version)
{
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *base_id = mf_manifest_get_base_id(manifest);
    const char *base_identity = mf_manifest_get_base_identity(manifest);

    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->base_id = g_strdup(base_id);
        ret_detail->base_version = g_strdup(base_version);
        ret_detail->identity = g_strdup(base_identity);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_MISSING_BASE,
                "%s needs base %s %s (identity %s), which is not installed",
                component_id,
                base_id,
                base_version,
                base_identity);
}

/**
 * mfd_set_error_id_conflict:
 * @installed: the installed version that has the same ID, but is of a different kind
 */
static void
mfd_set_error_id_conflict(GError **error, MfdErrorDetail *ret_detail, MfComponent *installed)
{
    const char *component_id = mf_component_get_id(installed);
    MfComponentKind kind = mf_component_get_kind(installed);
    gboolean sdk = mf_component_is_sdk(installed);

    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->kind = kind;
        ret_detail->sdk = sdk;
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_ID_CONFLICT,
                "%s is installed already as another kind of component (%s%s)",
                component_id,
                mf_component_kind_to_string(kind),
                sdk ? " SDK" : "");
}

/**
 * mfd_set_error_wrong_architecture:
 */
static void
mfd_set_error_wrong_architecture(GError **error,
                                 MfdErrorDetail *ret_detail,
                                 const char *component_id,
                                 const char *architecture)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->architecture = g_strdup(architecture);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_WRONG_ARCHITECTURE,
                "%s is built for %s, but this system is %s",
                component_id,
                architecture,
                mf_get_host_architecture());
}

/**
 * mfd_manager_becomes_current:
 *
 * Returns: %TRUE unless a newer version of the component is already installed
 */
static gboolean
mfd_manager_becomes_current(MfdManager *manager, MfManifest *manifest)
{
    g_autoptr(MfComponent)
        newest = mf_store_get_component(manager->store, mf_manifest_get_component_id(manifest), NULL, NULL);

    return newest == NULL || mf_version_compare_with_rev(mf_manifest_get_version(manifest),
                                                         mf_manifest_get_revision(manifest),
                                                         mf_component_get_version(newest),
                                                         mf_component_get_revision(newest)) >= 0;
}

/**
 * mfd_check_if_item_wins:
 * @manifest: the manifest of an item of the set that provides an identity
 * @installed: (nullable): the installed component that provides the same identity, or %NULL
 *
 * Check whether we use an item of the set to provide an identity, instead of
 * the installed component that provides the same identity. The item wins if
 * no installed component provides the identity, if the installed component
 * has a different ID, or if the installed component is an older version of
 * the item.
 *
 * Returns: %TRUE if the item of the set wins
 */
static gboolean
mfd_check_if_item_wins(MfManifest *manifest, MfComponent *installed)
{
    return installed == NULL ||
           g_strcmp0(mf_component_get_id(installed), mf_manifest_get_component_id(manifest)) != 0 ||
           mf_version_compare_with_rev(mf_component_get_version(installed),
                                       mf_component_get_revision(installed),
                                       mf_manifest_get_version(manifest),
                                       mf_manifest_get_revision(manifest)) < 0;
}

/**
 * mfd_manager_resolve_dependency:
 * @iplan: the plan whose base to set
 * @ret_kind: (out): the kind of the component that provides the identity
 * @ret_sdk: (out): whether the component that provides the identity is an SDK
 *
 * Find the component that provides the identity a component needs as its
 * base, in the set or in the store, and make it the base of @iplan.
 *
 * The base link follows the `current` version of that component if the
 * version we found will be the current one once the set is installed.
 * Otherwise, it names the exact version we found.
 *
 * Returns: %TRUE if we found a component that provides the identity
 */
static gboolean
mfd_manager_resolve_dependency(MfdManager *manager,
                               MfdInstallSet *set,
                               const char *identity,
                               MfdInstallPlan *iplan,
                               MfComponentKind *ret_kind,
                               gboolean *ret_sdk)
{
    MfdInstallItem *item = mfd_install_set_find_identity(set, identity);
    g_autoptr(MfComponent) installed = mf_store_find_identity(manager->store, identity, NULL);
    gboolean current;

    if (item != NULL && mfd_check_if_item_wins(item->manifest, installed)) {
        MfManifest *manifest = item->manifest;
        const char *component_id = mf_manifest_get_component_id(manifest);
        g_autofree char *version_name = NULL;

        current = mfd_manager_becomes_current(manager, manifest);
        version_name = current ? NULL
                               : mf_build_full_version(mf_manifest_get_version(manifest),
                                                       mf_manifest_get_revision(manifest));
        *ret_kind = mf_manifest_get_kind(manifest);
        *ret_sdk = mf_manifest_is_sdk(manifest);
        iplan->base = mf_base_link_new(mf_store_subdir_for_kind(*ret_kind, *ret_sdk),
                                       component_id,
                                       version_name);
        return TRUE;
    }

    if (installed == NULL)
        return FALSE;
    *ret_kind = mf_component_get_kind(installed);
    *ret_sdk = mf_component_is_sdk(installed);
    iplan->base = mf_base_link_new(
        mf_store_subdir_for_kind(*ret_kind, *ret_sdk),
        mf_component_get_id(installed),
        mf_component_is_current(installed) ? NULL : mf_component_get_full_version(installed));
    return TRUE;
}

/**
 * mfd_manager_get_installed:
 *
 * Returns: (transfer full) (nullable): the installed component with the given version and revision
 */
static MfComponent *
mfd_manager_get_installed(MfdManager *manager,
                          const char *component_id,
                          const char *version,
                          guint64 revision)
{
    g_autoptr(GPtrArray) versions = NULL;

    versions = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, NULL);
    for (guint i = 0; versions != NULL && i < versions->len; i++) {
        MfComponent *component = g_ptr_array_index(versions, i);

        if (g_str_equal(mf_component_get_version(component), version) &&
            mf_component_get_revision(component) == revision)
            return g_object_ref(component);
    }
    return NULL;
}

/**
 * mfd_manager_find_other_kind:
 *
 * An ID names exactly one kind of component.
 * This method searches the installed components for ones that
 * have the same ID as @manifest, but have a different kind.
 *
 * Returns: (transfer full) (nullable): an installed version with the same ID as @manifest, but of a different kind
 */
static MfComponent *
mfd_manager_find_other_kind(MfdManager *manager, MfManifest *manifest)
{
    g_autoptr(GPtrArray) versions = NULL;

    versions = mf_store_list_components(manager->store,
                                        mf_manifest_get_component_id(manifest),
                                        MF_COMPONENT_KIND_UNKNOWN,
                                        NULL);
    for (guint i = 0; versions != NULL && i < versions->len; i++) {
        MfComponent *component = g_ptr_array_index(versions, i);

        if (mf_component_get_kind(component) != mf_manifest_get_kind(manifest) ||
            mf_component_is_sdk(component) != mf_manifest_is_sdk(manifest))
            return g_object_ref(component);
    }
    return NULL;
}

/**
 * mfd_manager_resolve_exact_base:
 * @manifest: the manifest of the component whose base to find
 * @iplan: the plan whose base to set
 * @ret_kind: (out): the kind of the base
 * @ret_sdk: (out): whether the base is an SDK
 *
 * Find the exact version of the base a component was built against, in the
 * set or in the store, and make it the base of @iplan. Another version
 * that provides the same identity will not do.
 *
 * Returns: %TRUE if that version is there, and provides the identity the
 *   component needs.
 */
static gboolean
mfd_manager_resolve_exact_base(MfdManager *manager,
                               MfdInstallSet *set,
                               MfManifest *manifest,
                               MfdInstallPlan *iplan,
                               MfComponentKind *ret_kind,
                               gboolean *ret_sdk)
{
    const char *base_id = mf_manifest_get_base_id(manifest);
    const char *base_version = mf_manifest_get_base_version(manifest);
    guint64 base_revision = mf_manifest_get_base_revision(manifest);
    MfdInstallItem *in_set = mfd_install_set_find_exact_base(set, manifest);
    g_autoptr(MfComponent) installed = NULL;
    g_autofree char *version_name = NULL;
    const char *identity;
    MfComponentKind kind;
    gboolean sdk;

    if (in_set != NULL) {
        identity = mf_manifest_get_identity(in_set->manifest);
        kind = mf_manifest_get_kind(in_set->manifest);
        sdk = mf_manifest_is_sdk(in_set->manifest);
    } else {
        installed = mfd_manager_get_installed(manager, base_id, base_version, base_revision);
        if (installed == NULL)
            return FALSE;
        identity = mf_component_get_identity(installed);
        kind = mf_component_get_kind(installed);
        sdk = mf_component_is_sdk(installed);
    }
    if (g_strcmp0(identity, mf_manifest_get_base_identity(manifest)) != 0)
        return FALSE;

    version_name = mf_build_full_version(base_version, base_revision);
    iplan->base = mf_base_link_new(mf_store_subdir_for_kind(kind, sdk), base_id, version_name);
    *ret_kind = kind;
    *ret_sdk = sdk;
    return TRUE;
}

/**
 * mfd_manager_plan_item_install:
 *
 * Plan how we install an item of the set: whether we add its image to the
 * store, whether the component is installed already or not needed at all,
 * its install mode, and what it builds on. Everything that refuses the
 * installation of the item is checked here, before anything is written.
 *
 * Returns: %TRUE if the item can be installed
 */
static gboolean
mfd_manager_plan_item_install(MfdManager *manager,
                              MfdInstallSet *set,
                              MfdInstallItem *item,
                              MfdInstallPlan *iplan,
                              MfdErrorDetail *ret_detail,
                              GError **error)
{
    MfManifest *manifest = item->manifest;
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *version = mf_manifest_get_version(manifest);
    const char *verity_hash = mf_manifest_get_verity_hash(manifest);
    const char *base_id = mf_manifest_get_base_id(manifest);
    MfComponentKind kind = mf_manifest_get_kind(manifest);
    gboolean sdk = mf_manifest_is_sdk(manifest);
    gboolean dependency = item->is_dependency;
    g_autoptr(MfComponent) other_kind = NULL;
    g_autoptr(MfComponent) existing = NULL;

    if (!g_str_equal(mf_manifest_get_architecture(manifest), mf_get_host_architecture())) {
        mfd_set_error_wrong_architecture(error,
                                         ret_detail,
                                         component_id,
                                         mf_manifest_get_architecture(manifest));
        return FALSE;
    }
    other_kind = mfd_manager_find_other_kind(manager, manifest);
    if (other_kind != NULL) {
        mfd_set_error_id_conflict(error, ret_detail, other_kind);
        return FALSE;
    }
    if (!mf_manifest_check_integration(manifest, error))
        return FALSE;

    iplan->item = item;
    iplan->add_image = !mf_store_has_image(manager->store, verity_hash);
    iplan->mode = dependency || mf_component_kind_is_always_auto(kind, sdk) ? MF_INSTALL_MODE_AUTO
                                                                            : MF_INSTALL_MODE_MANUAL;

    existing = mfd_manager_get_installed(manager, component_id, version, mf_manifest_get_revision(manifest));
    if (existing != NULL) {
        if (g_strcmp0(mf_component_get_verity_hash(existing), verity_hash) != 0) {
            g_warning("%s %s is installed already, with another image: %s instead of %s",
                      component_id,
                      version,
                      mf_component_get_verity_hash(existing),
                      verity_hash);
            mfd_set_error_already_installed(error, ret_detail, component_id, version);
            return FALSE;
        }
        if (dependency) {
            /* already installed (how it got there doesn't matter) */
            iplan->skip = TRUE;
            return TRUE;
        }
        /* only a runtime with the install mode auto can be installed again, which makes it manual */
        if (mf_component_kind_is_always_auto(kind, sdk) ||
            mf_component_get_install_mode(existing) != MF_INSTALL_MODE_AUTO) {
            mfd_set_error_already_installed(error, ret_detail, component_id, version);
            return FALSE;
        }
        g_info("%s %s was a dependency, and is installed on purpose now", component_id, version);
    }

    /* a kit that came along with an application is not needed if the application can run on
     * a newer version that we already have in the store */
    if (dependency) {
        g_autoptr(MfComponent)
            provider = mf_store_find_identity(manager->store, mf_manifest_get_identity(manifest), NULL);

        if (!mfd_check_if_item_wins(manifest, provider)) {
            g_info("%s %s is not needed, %s is installed",
                   component_id,
                   version,
                   mf_component_get_full_version(provider));
            iplan->skip = TRUE;
            iplan->add_image = FALSE;
            return TRUE;
        }
    }

    if (base_id != NULL) {
        const char *base_version = mf_manifest_get_base_version(manifest);
        MfComponentKind base_kind;
        gboolean base_sdk;

        /* a platform and an SDK need the exact version they were built against,
         * everything else builds on whatever provides the identity of its base */
        if (sdk || kind == MF_COMPONENT_KIND_PLATFORM) {
            if (!mfd_manager_resolve_exact_base(manager, set, manifest, iplan, &base_kind, &base_sdk)) {
                g_autofree char *base_version_name = mf_build_full_version(
                    base_version,
                    mf_manifest_get_base_revision(manifest));

                mfd_set_error_missing_base(error, ret_detail, manifest, base_version_name);
                return FALSE;
            }
        } else if (!mfd_manager_resolve_dependency(manager,
                                                   set,
                                                   mf_manifest_get_base_identity(manifest),
                                                   iplan,
                                                   &base_kind,
                                                   &base_sdk)) {
            mfd_set_error_missing_base(error, ret_detail, manifest, base_version);
            return FALSE;
        }

        /* a component can only build on the kinds of layer that are allowed below it */
        if (!mf_component_kind_can_build_on(kind, sdk, base_kind, base_sdk)) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        "%s: %s%s %s can not build on %s%s %s",
                        item->name,
                        mf_component_kind_to_string(kind),
                        sdk ? " SDK" : "",
                        component_id,
                        mf_component_kind_to_string(base_kind),
                        base_sdk ? " SDK" : "",
                        iplan->base->component_id);
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * mfd_removal_item_free:
 */
static void
mfd_removal_item_free(MfdRemovalItem *item)
{
    g_object_unref(item->component);
    g_free(item);
}

/**
 * mfd_removal_plan_free:
 */
void
mfd_removal_plan_free(MfdRemovalPlan *rmplan)
{
    if (rmplan == NULL)
        return;

    g_hash_table_unref(rmplan->cpts_set);
    g_ptr_array_unref(rmplan->items);
    g_ptr_array_unref(rmplan->images);
    g_free(rmplan);
}

/**
 * mfd_removal_plan_new:
 *
 * Returns: (transfer full): an empty removal plan
 */
static MfdRemovalPlan *
mfd_removal_plan_new(void)
{
    MfdRemovalPlan *rmplan = g_new0(MfdRemovalPlan, 1);

    rmplan->items = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_removal_item_free);
    /* the set borrows the components, which are owned by the items */
    rmplan->cpts_set = g_hash_table_new(g_direct_hash, g_direct_equal);
    rmplan->images = g_ptr_array_new_with_free_func(g_free);
    return rmplan;
}

/**
 * mfd_removal_plan_add_item:
 * @rmplan: the #MfdRemovalPlan
 * @component: the version that will be removed
 * @action: the reason for the removal, see #MfdRemovalItem
 *
 * Add a component to a removal plan.
 */
static void
mfd_removal_plan_add_item(MfdRemovalPlan *rmplan, MfComponent *component, MfAction action)
{
    MfdRemovalItem *item = g_new0(MfdRemovalItem, 1);

    item->component = g_object_ref(component);
    item->action = action;
    g_ptr_array_add(rmplan->items, item);
    g_hash_table_add(rmplan->cpts_set, item->component);
}

/**
 * mfd_manager_find_used:
 * @installed: (element-type MfComponent): every installed version
 * @rm_pending: (element-type MfComponent): versions that should be ignored because they will be removed
 *
 * Find the versions that the `base` links of the other installed versions
 * point to. A link to `current` keeps the version that `current` points to
 * now: if we removed it, `current` would move to an older version, and the
 * versions that link to it would build on that one. We do not keep a base
 * whose link we can not read, or that is not installed.
 *
 * Returns: every version a link points to, mapped to a version that links to it
 */
static GHashTable *
mfd_manager_find_used(MfdManager *manager, GPtrArray *installed, GHashTable *rm_pending)
{
    GHashTable *used = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, NULL);

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);
        MfComponent *base;

        if (g_hash_table_contains(rm_pending, component))
            continue;
        base = mf_store_get_base(manager->store, component, NULL);
        if (base != NULL)
            g_hash_table_insert(used, base, component);
    }

    return used;
}

/**
 * mfd_find_used_images:
 * @installed: (element-type MfComponent): every installed version
 * @rm_pending: (element-type MfComponent): versions that are planned for removal
 *
 * Returns: a set with the verity hash of every image that a version outside of @rm_pending uses
 */
static GHashTable *
mfd_find_used_images(GPtrArray *installed, GHashTable *rm_pending)
{
    GHashTable *used = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);

        if (!g_hash_table_contains(rm_pending, component))
            g_hash_table_add(used, (gpointer) mf_component_get_verity_hash(component));
    }

    return used;
}

/**
 * mfd_find_newest_apps:
 * @installed: (element-type MfComponent): every installed version
 * @rm_pending: (element-type MfComponent): versions that are planned for removal
 *
 * Find the newest version of every application and plugin that is not
 * planned for removal. The newest version is kept regardless of its install mode.
 *
 * Returns: a set of the newest versions
 */
static GHashTable *
mfd_find_newest_apps(GPtrArray *installed, GHashTable *rm_pending)
{
    /* maps a component ID to its newest version */
    g_autoptr(GHashTable) newest = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *kept = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTableIter iter;
    gpointer value;

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);
        MfComponent *other;

        if (!mf_component_kind_is_always_auto(mf_component_get_kind(component),
                                              mf_component_is_sdk(component)) ||
            g_hash_table_contains(rm_pending, component))
            continue;
        other = g_hash_table_lookup(newest, mf_component_get_id(component));
        if (other == NULL || mf_version_compare_with_rev(mf_component_get_version(component),
                                                         mf_component_get_revision(component),
                                                         mf_component_get_version(other),
                                                         mf_component_get_revision(other)) > 0)
            g_hash_table_insert(newest, (gpointer) mf_component_get_id(component), component);
    }

    g_hash_table_iter_init(&iter, newest);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_hash_table_add(kept, value);

    return kept;
}

/**
 * mfd_manager_complete_removal_plan:
 * @rmplan: the removal plan, which may already list versions planned for removal
 * @installed: (element-type MfComponent): every installed version
 * @requested: (element-type MfComponent): the versions that were requested to be removed
 *
 * Work out everything a removal will delete, in the order it has to go.
 *
 * A version is planned for removal if deletion was requested explicitly, or if it is
 * marked as auto and is not the newest version of an application or plugin. In both cases,
 * the version is only planned for removal once no kept version links to it anymore.
 * Removing a version can leave its base unused, e.g. the kit of an
 * application, so the search is repeated until no more versions are found.
 * A version is therefore always removed after every version that links to it.
 * A requested version that a kept version still links to is not planned for
 * removal.
 *
 * Then every image that no kept version uses is planned for removal, too.
 */
static gboolean
mfd_manager_complete_removal_plan(MfdManager *manager,
                                  MfdRemovalPlan *rmplan,
                                  GPtrArray *installed,
                                  GHashTable *requested,
                                  GError **error)
{
    g_autoptr(GHashTable) newest_apps = NULL;
    g_autoptr(GHashTable) images_used = NULL;
    g_autoptr(GPtrArray) images = NULL;
    gboolean added;

    newest_apps = mfd_find_newest_apps(installed, requested);
    do {
        /* the versions that are still used by a version which is not planned for removal */
        g_autoptr(GHashTable) used = mfd_manager_find_used(manager, installed, rmplan->cpts_set);

        added = FALSE;
        for (guint i = 0; i < installed->len; i++) {
            MfComponent *component = g_ptr_array_index(installed, i);

            if (g_hash_table_contains(rmplan->cpts_set, component) || g_hash_table_contains(used, component))
                continue;
            if (g_hash_table_contains(requested, component)) {
                mfd_removal_plan_add_item(rmplan, component, MF_ACTION_REMOVE);
            } else if (mf_component_get_install_mode(component) == MF_INSTALL_MODE_AUTO &&
                       !g_hash_table_contains(newest_apps, component)) {
                g_info("%s is not needed anymore", mf_component_get_dir(component));
                mfd_removal_plan_add_item(rmplan, component, MF_ACTION_CLEANUP);
            } else {
                /* kept: marked manual, or the newest version of an application or plugin */
                continue;
            }
            added = TRUE;
        }
    } while (added);

    /* plan to remove the images that no kept version uses */
    images_used = mfd_find_used_images(installed, rmplan->cpts_set);
    images = mf_store_list_images(manager->store, error);
    if (images == NULL)
        return FALSE;
    for (guint i = 0; i < images->len; i++) {
        const char *hash = g_ptr_array_index(images, i);
        g_autofree char *path = NULL;
        struct stat st;

        if (g_hash_table_contains(images_used, hash))
            continue;
        path = mf_store_image_path_at(mf_store_get_path(manager->store), hash);
        if (stat(path, &st) == 0)
            rmplan->image_bytes += (guint64) st.st_size;
        g_ptr_array_add(rmplan->images, g_strdup(hash));
    }

    return TRUE;
}

/**
 * mfd_version_selects:
 * @version: a version, or a full version if @full is set
 *
 * Check whether @version matches the version of an installed component.
 */
static gboolean
mfd_version_selects(MfComponent *component, const char *version, gboolean full)
{
    return g_str_equal(full ? mf_component_get_full_version(component) : mf_component_get_version(component),
                       version);
}

/**
 * mfd_manager_resolve_version:
 * @version: (nullable): a version, which selects all of its revisions, or
 *   a full version (`<version>-<rev>`), which selects one revision
 * @ret_full: (out): whether @version is a full version
 *
 * Find out whether @version is a version or a full version, by comparing it
 * with the installed versions of the component.
 *
 * A version may itself end in a dash and a number, like a full version does.
 * We therefore refuse a @version that matches the version of one installed
 * component, and the full version of a different one.
 */
static gboolean
mfd_manager_resolve_version(MfdManager *manager,
                            const char *component_id,
                            const char *version,
                            gboolean *ret_full,
                            GError **error)
{
    g_autoptr(GPtrArray) components = NULL;
    gboolean as_version = FALSE;
    gboolean as_full = FALSE;

    *ret_full = FALSE;
    if (version == NULL)
        return TRUE;
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return FALSE;
    for (guint i = 0; i < components->len; i++) {
        auto component = MF_COMPONENT (g_ptr_array_index(components, i));

        as_version |= mfd_version_selects(component, version, FALSE);
        as_full |= mfd_version_selects(component, version, TRUE);
    }
    if (as_version && as_full) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "%s %s is ambiguous: It matches both the version of an installed component, as well as its version and revision",
                    component_id,
                    version);
        return FALSE;
    }

    *ret_full = as_full;
    return TRUE;
}

/**
 * mfd_manager_plan_removal:
 * @manager: an #MfdManager
 * @component_id: (nullable): a component to remove, or %NULL to only plan a cleanup
 * @version: (nullable): the version to remove with all of its revisions, the
 *   full version (`<version>-<rev>`) to remove one revision, or %NULL for all versions
 * @ret_detail: (out caller-allocates) (optional): details about the failure
 * @error: a #GError
 *
 * Create the removal plan for a component. The plan contains the requested
 * versions of the component, every dependency that no kept component builds
 * on after their removal, and every image that no kept component uses.
 * The store is not changed.
 *
 * Returns: (transfer full): the plan, or %NULL on error
 */
MfdRemovalPlan *
mfd_manager_plan_removal(MfdManager *manager,
                         const char *component_id,
                         const char *version,
                         MfdErrorDetail *ret_detail,
                         GError **error)
{
    g_autoptr(MfdRemovalPlan) rmplan = mfd_removal_plan_new();
    g_autoptr(GPtrArray) installed = NULL;
    g_autoptr(GHashTable) requested = g_hash_table_new(g_direct_hash, g_direct_equal);
    gboolean full = FALSE;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    installed = mf_store_list_components(manager->store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
    if (installed == NULL)
        return NULL;

    if (component_id != NULL) {
        if (!mfd_manager_resolve_version(manager, component_id, version, &full, error))
            return NULL;
        for (guint i = 0; i < installed->len; i++) {
            MfComponent *candidate = g_ptr_array_index(installed, i);

            if (g_str_equal(mf_component_get_id(candidate), component_id) &&
                (version == NULL || mfd_version_selects(candidate, version, full)))
                g_hash_table_add(requested, candidate);
        }
        if (g_hash_table_size(requested) == 0) {
            mfd_set_error_not_installed(error, ret_detail, component_id, version);
            return NULL;
        }
    }

    if (!mfd_manager_complete_removal_plan(manager, rmplan, installed, requested, error))
        return NULL;

    /* a requested version that is not planned for removal is still linked to by a kept version */
    for (guint i = 0; i < installed->len; i++) {
        MfComponent *candidate = g_ptr_array_index(installed, i);
        g_autoptr(GHashTable) used = NULL;

        if (!g_hash_table_contains(requested, candidate) ||
            g_hash_table_contains(rmplan->cpts_set, candidate))
            continue;
        used = mfd_manager_find_used(manager, installed, rmplan->cpts_set);
        mfd_set_error_in_use(error,
                             ret_detail,
                             component_id,
                             mf_component_get_id(g_hash_table_lookup(used, candidate)));
        return NULL;
    }

    return g_steal_pointer(&rmplan);
}

/**
 * mfd_manager_plan_supersede:
 * @plans: (element-type MfdInstallPlan): the plans of the installation
 *
 * Create the removal plan for the versions that an installation supersedes.
 *
 * A superseded version is an older version or revision of a component that
 * was installed, which is marked as auto and which no kept version links to.
 * As with any removal, the plan then also contains every dependency that no
 * kept component builds on anymore, and every image that no kept component
 * uses. The store is not changed.
 *
 * Returns: (transfer full): the plan, or %NULL on error
 */
static MfdRemovalPlan *
mfd_manager_plan_supersede(MfdManager *manager, GArray *plans, GError **error)
{
    g_autoptr(MfdRemovalPlan) rmplan = mfd_removal_plan_new();
    g_autoptr(GPtrArray) installed = NULL;
    g_autoptr(GHashTable) superseded = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_autoptr(GHashTable) used = NULL;
    gboolean kept;

    installed = mf_store_list_components(manager->store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
    if (installed == NULL)
        return NULL;

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);

        if (mf_component_get_install_mode(component) != MF_INSTALL_MODE_AUTO)
            continue;
        for (guint j = 0; j < plans->len; j++) {
            MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, j);
            MfManifest *manifest = plan->item->manifest;

            if (!plan->skip &&
                g_str_equal(mf_component_get_id(component), mf_manifest_get_component_id(manifest)) &&
                mf_version_compare_with_rev(mf_component_get_version(component),
                                            mf_component_get_revision(component),
                                            mf_manifest_get_version(manifest),
                                            mf_manifest_get_revision(manifest)) < 0) {
                g_hash_table_add(superseded, component);
                break;
            }
        }
    }

    /* a superseded version is kept if a kept version links to it. We repeat the check until
     * nothing changes, as a version that is kept now may itself link to a superseded version. */
    do {
        GHashTableIter iter;
        gpointer component;

        g_clear_pointer(&used, g_hash_table_unref);
        used = mfd_manager_find_used(manager, installed, superseded);
        kept = FALSE;
        g_hash_table_iter_init(&iter, superseded);
        while (g_hash_table_iter_next(&iter, &component, NULL)) {
            MfComponent *used_by = g_hash_table_lookup(used, component);

            if (used_by == NULL)
                continue;
            g_info("%s is superseded, but %s still builds on it",
                   mf_component_get_dir(component),
                   mf_component_get_id(used_by));
            g_hash_table_iter_remove(&iter);
            kept = TRUE;
        }
    } while (kept);

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);

        if (g_hash_table_contains(superseded, component))
            mfd_removal_plan_add_item(rmplan, component, MF_ACTION_CLEANUP);
    }

    /* if no version is superseded, we do not clean up anything else either */
    if (rmplan->items->len > 0 &&
        !mfd_manager_complete_removal_plan(manager, rmplan, installed, superseded, error))
        return NULL;
    return g_steal_pointer(&rmplan);
}

/**
 * mfd_manager_execute_removal:
 * @result: where to add what was removed
 *
 * Execute a removal plan: remove the version directories first, then the images.
 *
 * Progress is reported for the components only.
 */
static gboolean
mfd_manager_execute_removal(MfdManager *manager,
                            MfdRemovalPlan *rmplan,
                            MfdProgressTracker *tracker,
                            MfResult *result,
                            GError **error)
{
    for (guint i = 0; i < rmplan->items->len; i++) {
        MfdRemovalItem *item = g_ptr_array_index(rmplan->items, i);

        mfd_progress_begin_item(tracker,
                                item->action,
                                mf_component_get_id(item->component),
                                mf_component_get_full_version(item->component),
                                0);
        if (!mfd_store_writer_remove_component(manager->writer, item->component, error))
            return FALSE;
        mfd_progress_end_item(tracker);
        mf_result_add_removed(result, item->component);
    }

    /* update the exports of the applications on the host, as their current version may have
     * changed. A failed update is only a warning. */
    for (guint i = 0; i < rmplan->items->len; i++) {
        MfdRemovalItem *item = g_ptr_array_index(rmplan->items, i);
        g_autoptr(GError) reconcile_error = NULL;

        if (mf_component_get_kind(item->component) == MF_COMPONENT_KIND_APP &&
            !mfd_integrator_reconcile(manager->integrator,
                                      result,
                                      mf_component_get_id(item->component),
                                      &reconcile_error))
            mfd_emit_warning(result,
                             "Unable to update the host files of %s: %s",
                             mf_component_get_id(item->component),
                             reconcile_error->message);
    }

    for (guint i = 0; i < rmplan->images->len; i++) {
        const char *hash = g_ptr_array_index(rmplan->images, i);

        if (!mfd_store_writer_remove_image(manager->writer, hash, error))
            return FALSE;
        mf_result_add_removed_image(result, hash);
    }
    mf_result_set_bytes_freed(result, rmplan->image_bytes);

    return TRUE;
}

/**
 * mfd_make_local_appstream_catalog:
 *
 * Create catalog data for a component of the local origin. We drop its
 * screenshots (we only allow those for repos).
 */
static GBytes *
mfd_make_local_appstream_catalog(GBytes *catalog, GError **error)
{
    g_autoptr(AsMetadata) metadata = as_metadata_new();
    AsComponentBox *components;
    gchar *xml;

    as_metadata_set_locale(metadata, "ALL");
    as_metadata_set_format_style(metadata, AS_FORMAT_STYLE_CATALOG);
    if (!as_metadata_parse_bytes(metadata, catalog, AS_FORMAT_KIND_XML, error))
        return NULL;

    components = as_metadata_get_components(metadata);
    for (guint i = 0; i < as_component_box_len(components); i++) {
        AsComponent *component = as_component_box_index(components, i);

        /* AppStream has no function to remove screenshots, so we empty the array of the component */
        g_ptr_array_set_size(as_component_get_screenshots_all(component), 0);
    }

    as_metadata_set_origin(metadata, MF_BUNDLE_CATALOG_ORIGIN);
    xml = as_metadata_components_to_catalog(metadata, AS_FORMAT_KIND_XML, error);
    if (xml == NULL)
        return NULL;
    return g_bytes_new_take(xml, strlen(xml));
}

/**
 * mfd_manager_read_exports:
 * @manifest: the manifest of an image in the store
 *
 * Read the integration files of an image, and check that they can be
 * installed on the host.
 *
 * Returns: (transfer full) (element-type utf8 GBytes): the contents of the
 *   files by their export path, or %NULL on error
 */
static GHashTable *
mfd_manager_read_exports(MfdManager *manager, MfManifest *manifest, GError **error)
{
    MfIntegration *integration = mf_manifest_get_integration(manifest);
    g_autoptr(GHashTable) exports = mfd_image_read_integration(manager->store, manifest, error);
    GPtrArray *files;

    if (exports == NULL)
        return NULL;
    if (integration == NULL)
        return g_steal_pointer(&exports);

    files = mf_integration_get_files(integration);
    for (guint i = 0; i < files->len; i++) {
        MfIntegrationFile *file = g_ptr_array_index(files, i);
        GBytes *content = g_hash_table_lookup(exports, mf_integration_file_get_export_path(file));
        g_autoptr(GBytes) rendered = mf_integration_file_render(file, manifest, content, error);

        if (rendered == NULL)
            return NULL;
    }

    return g_steal_pointer(&exports);
}

/**
 * mfd_manager_execute_install_plan:
 * @iplan: the plan of the component
 * @tracker: the progress of the installation
 * @ret_bytes_added: (out): the size of the image if we added it to the store, or 0
 * @ret_detail: (out caller-allocates) (optional): details about the failure
 * @error: a #GError
 *
 * Install one component of a set: write its image into the store, unless
 * the store has it already, and then record the component.
 *
 * If the component can not be recorded, an image that we added is removed again.
 *
 * Returns: %TRUE if the component was installed
 */
static gboolean
mfd_manager_execute_install_plan(MfdManager *manager,
                                 MfdInstallPlan *iplan,
                                 MfdProgressTracker *tracker,
                                 MfResult *result,
                                 guint64 *ret_bytes_added,
                                 MfdErrorDetail *ret_detail,
                                 GError **error)
{
    MfdInstallItem *item = iplan->item;
    MfManifest *manifest = item->manifest;
    const char *verity_hash = mf_manifest_get_verity_hash(manifest);
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *version = mf_manifest_get_version(manifest);
    g_autofree char *full_version = mf_build_full_version(version, mf_manifest_get_revision(manifest));
    guint64 image_size = mf_manifest_get_image_size(manifest);
    g_autoptr(GBytes) catalog = NULL;
    g_autoptr(GHashTable) exports = NULL;
    g_autoptr(GError) local_error = NULL;
    gboolean added = FALSE;

    *ret_bytes_added = 0;
    /* build the catalog data for the local origin first, as that can fail */
    catalog = mfd_make_local_appstream_catalog(item->catalog, error);
    if (catalog == NULL) {
        g_prefix_error(error, "%s: ", component_id);
        return FALSE;
    }

    if (iplan->add_image)
        mfd_progress_begin_item(tracker, MF_ACTION_INSTALL, component_id, full_version, image_size);

    /* a component of the set that was installed before may have added the same image already */
    if (iplan->add_image && !mf_store_has_image(manager->store, verity_hash)) {
        MfdItemImageWrite write = {item, tracker};

        g_info("Adding image %s of %s %s", verity_hash, component_id, version);
        if (!mfd_store_writer_add_image(manager->writer, manifest, mfd_write_item_image, &write, error))
            return FALSE;
        added = TRUE;
    } else {
        g_info("Image %s of %s %s is in the store already", verity_hash, component_id, version);
    }

    /* we can only read files from an image once it is in the store, as it is not verified before */
    exports = mfd_manager_read_exports(manager, manifest, &local_error);
    if (exports == NULL || !mfd_store_writer_install_component(manager->writer,
                                                               manifest,
                                                               iplan->base,
                                                               catalog,
                                                               item->icons,
                                                               exports,
                                                               item->origin,
                                                               iplan->mode,
                                                               &local_error)) {
        g_autoptr(GError) remove_error = NULL;

        /* the same version was installed in the meantime, and it uses the image, so we keep it */
        if (g_error_matches(local_error, MF_ERROR, MF_ERROR_ALREADY_INSTALLED)) {
            mfd_set_error_already_installed(error, ret_detail, component_id, version);
            return FALSE;
        }
        if (added && !mfd_store_writer_remove_image(manager->writer, verity_hash, &remove_error))
            g_warning("Unable to remove image %s again, the next cleanup will remove it: %s",
                      verity_hash,
                      remove_error->message);
        g_propagate_error(error, g_steal_pointer(&local_error));
        return FALSE;
    }

    /* the component is installed already, so a failed update of its exports is only a warning */
    if (mf_manifest_get_kind(manifest) == MF_COMPONENT_KIND_APP &&
        !mfd_integrator_reconcile(manager->integrator, result, component_id, &local_error))
        mfd_emit_warning(result,
                         "Unable to update the host files of %s: %s",
                         component_id,
                         local_error->message);

    if (iplan->add_image)
        mfd_progress_end_item(tracker);
    if (added)
        *ret_bytes_added = image_size;
    return TRUE;
}

/**
 * mfd_manager_check_file_conflicts:
 * @plans: (element-type MfdInstallPlan): the plans of the installation
 *
 * Check that the integration files of the installation do not conflict with
 * files that exist on the host. Only the integration files of the current
 * version of a component are installed on the host, so we only check the
 * versions that become current.
 *
 * Returns: %TRUE if there is no conflict
 */
static gboolean
mfd_manager_check_file_conflicts(MfdManager *manager, GArray *plans, GError **error)
{
    g_autoptr(GPtrArray) manifests = g_ptr_array_sized_new(plans->len);

    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        MfManifest *manifest = plan->item->manifest;

        if (!plan->skip && mfd_manager_becomes_current(manager, manifest))
            g_ptr_array_add(manifests, manifest);
    }

    return mfd_integrator_check_conflicts(manager->integrator, manifests, error);
}

/**
 * mfd_manager_plan_install:
 * @ret_detail: (out caller-allocates) (optional): details about the failure
 *
 * Plan the installation of every item of a set.
 *
 * Returns: (transfer full) (element-type MfdInstallPlan): the plans, or %NULL
 *   if the set can not be installed
 */
static GArray *
mfd_manager_plan_install(MfdManager *manager, MfdInstallSet *set, MfdErrorDetail *ret_detail, GError **error)
{
    GPtrArray *items = set->items;
    g_autoptr(GArray) plans = g_array_sized_new(FALSE, TRUE, sizeof(MfdInstallPlan), items->len);

    g_array_set_size(plans, items->len);
    g_array_set_clear_func(plans, (GDestroyNotify) mfd_install_plan_clear);
    for (guint i = 0; i < items->len; i++) {
        if (!mfd_manager_plan_item_install(manager,
                                           set,
                                           g_ptr_array_index(items, i),
                                           &g_array_index(plans, MfdInstallPlan, i),
                                           ret_detail,
                                           error))
            return NULL;
    }
    if (!mfd_manager_check_file_conflicts(manager, plans, error))
        return NULL;

    /* install every component after the components that it builds on */
    g_array_sort(plans, mfd_install_plan_compare_layer);

    return g_steal_pointer(&plans);
}

/**
 * mfd_manager_results_add_installed:
 * @result: where to add what was installed
 * @plans: (element-type MfdInstallPlan): the plans of the installation
 *
 * Add the components of the set that the installation installed to
 * @result, but not the dependencies that were there already.
 */
static void
mfd_manager_results_add_installed(MfdManager *manager, MfResult *result, GArray *plans)
{
    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        MfManifest *manifest = plan->item->manifest;
        g_autoptr(MfComponent) component = NULL;

        if (plan->skip)
            continue;
        component = mfd_manager_get_installed(manager,
                                              mf_manifest_get_component_id(manifest),
                                              mf_manifest_get_version(manifest),
                                              mf_manifest_get_revision(manifest));
        if (component == NULL ||
            g_strcmp0(mf_component_get_verity_hash(component), mf_manifest_get_verity_hash(manifest)) != 0)
            continue;
        mf_result_add_installed(result, component);
    }
}

/**
 * mfd_manager_install_set:
 * @manager: an #MfdManager
 * @set: the components to install
 * @progress_func: (nullable): called whenever the operation progresses
 * @user_data: passed to @progress_func
 * @ret_detail: (out caller-allocates) (optional): details about the failure
 * @error: a #GError
 *
 * Install the components of a set.
 *
 * We plan the installation of every item first, and then install every
 * component after the components of the set that it builds on.
 *
 * If installing a component fails, we undo only the installation of that
 * component and stop. The components that were installed before stay
 * installed, so their images do not have to be copied (or downloaded) again.
 * Once all components are installed, we remove the superseded versions.
 *
 * Returns: (transfer full): the result of the installation, or %NULL on error
 */
MfResult *
mfd_manager_install_set(MfdManager *manager,
                        MfdInstallSet *set,
                        MfProgressFunc progress_func,
                        gpointer user_data,
                        MfdErrorDetail *ret_detail,
                        GError **error)
{
    g_auto(MfdProgressTracker) tracker = {
        .func = progress_func,
        .user_data = user_data,
        .progress = mf_progress_new(),
    };
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GArray) plans = NULL;
    g_autoptr(MfdRemovalPlan) rmplan = NULL;
    g_autoptr(GError) supersede_error = NULL;
    guint64 bytes_added = 0;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    if (!mfd_store_writer_ensure_root(manager->writer, error))
        return NULL;
    /* reload the store, to ensure all our information is up-to-date */
    if (!mf_store_load(manager->store, error))
        return NULL;

    mfd_integrator_reconcile_all(manager->integrator);

    /* plan installation */
    plans = mfd_manager_plan_install(manager, set, ret_detail, error);
    if (plans == NULL)
        return NULL;

    /* every image that we copy is one progress item, and removing the superseded versions
     * is one more item */
    tracker.total = 1;
    for (guint i = 0; i < plans->len; i++) {
        if (g_array_index(plans, MfdInstallPlan, i).add_image)
            tracker.total++;
    }
    result = mf_result_new();
    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        guint64 image_bytes;

        if (plan->skip)
            continue;
        if (!mfd_manager_execute_install_plan(manager,
                                              plan,
                                              &tracker,
                                              result,
                                              &image_bytes,
                                              ret_detail,
                                              error))
            return NULL;
        bytes_added += image_bytes;
    }

    /* remove the superseded versions. The installation is still successful if their removal fails. */
    rmplan = mfd_manager_plan_supersede(manager, plans, &supersede_error);
    mfd_progress_split_last_item(&tracker, rmplan != NULL ? rmplan->items->len : 0);
    if (rmplan == NULL || !mfd_manager_execute_removal(manager, rmplan, &tracker, result, &supersede_error))
        mfd_emit_warning(result, "Unable to remove the superseded versions: %s", supersede_error->message);

    mfd_manager_results_add_installed(manager, result, plans);
    mf_result_set_bytes_added(result, bytes_added);

    return g_steal_pointer(&result);
}

/**
 * mfd_manager_remove:
 * @manager: an #MfdManager
 * @component_id: (nullable): the component to remove, or %NULL to only do a cleanup
 * @version: (nullable): the version to remove with all of its revisions, the
 *   full version (`<version>-<rev>`) to remove one revision, or %NULL for all versions
 * @progress_func: (nullable): called whenever the operation progresses
 * @user_data: passed to @progress_func
 * @ret_detail: (out caller-allocates) (optional): details about the failure
 * @error: a #GError
 *
 * Remove a component, or one version of it. The removal is refused if a kept
 * version of a different component builds on it.
 *
 * Returns: (transfer full): the result of the removal, or %NULL on error
 */
MfResult *
mfd_manager_remove(MfdManager *manager,
                   const char *component_id,
                   const char *version,
                   MfProgressFunc progress_func,
                   gpointer user_data,
                   MfdErrorDetail *ret_detail,
                   GError **error)
{
    g_auto(MfdProgressTracker) tracker = {
        .func = progress_func,
        .user_data = user_data,
        .progress = mf_progress_new(),
    };
    g_autoptr(MfdRemovalPlan) rmplan = NULL;
    g_autoptr(MfResult) result = NULL;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    if (!mfd_store_writer_ensure_root(manager->writer, error))
        return NULL;

    /* reload the store, so the removal is planned for the components that are installed now */
    if (!mf_store_load(manager->store, error))
        return NULL;
    mfd_integrator_reconcile_all(manager->integrator);
    rmplan = mfd_manager_plan_removal(manager, component_id, version, ret_detail, error);
    if (rmplan == NULL)
        return NULL;

    tracker.total = rmplan->items->len;
    if (!mfd_store_writer_remove_cruft(manager->writer, error))
        return NULL;

    result = mf_result_new();
    if (!mfd_manager_execute_removal(manager, rmplan, &tracker, result, error))
        return NULL;

    return g_steal_pointer(&result);
}

/**
 * mfd_manager_mark_selects:
 *
 * Check whether @component is one of the versions to mark. If @version is
 * %NULL, only the current version is marked.
 */
static gboolean
mfd_manager_mark_selects(MfComponent *component, const char *version, gboolean full)
{
    if (version == NULL)
        return mf_component_is_current(component);
    return mfd_version_selects(component, version, full);
}

/**
 * mfd_manager_mark:
 * @manager: an #MfdManager
 * @component_id: the component to mark
 * @version: (nullable): the version to mark with all of its revisions, the
 *   full version (`<version>-<rev>`) to mark one revision, or %NULL for the current version
 * @mode: the install mode to record
 * @ret_detail: (out caller-allocates) (optional): details about the failure
 * @error: a #GError
 *
 * Mark versions of a component as kept until they are removed explicitly
 * (%MF_INSTALL_MODE_MANUAL), or as removed once they are not needed anymore
 * (%MF_INSTALL_MODE_AUTO). Marking does not remove anything. Versions that
 * are not needed anymore are removed by the next installation, removal or cleanup.
 *
 * Returns: (transfer full) (element-type MfComponent): the marked versions, or %NULL on error
 */
GPtrArray *
mfd_manager_mark(MfdManager *manager,
                 const char *component_id,
                 const char *version,
                 MfInstallMode mode,
                 MfdErrorDetail *ret_detail,
                 GError **error)
{
    g_autoptr(GPtrArray) components = NULL;
    g_autoptr(GPtrArray) marked = g_ptr_array_new_with_free_func(g_object_unref);
    gboolean full = FALSE;
    guint n_marked = 0;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);
    g_return_val_if_fail(mode == MF_INSTALL_MODE_MANUAL || mode == MF_INSTALL_MODE_AUTO, NULL);

    if (!mf_store_load(manager->store, error))
        return NULL;
    if (!mfd_manager_resolve_version(manager, component_id, version, &full, error))
        return NULL;
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (!mfd_manager_mark_selects(component, version, full))
            continue;
        if (!mfd_store_writer_set_mode(manager->writer, component, mode, error))
            return NULL;
        g_message("Marked %s as %s", mf_component_get_dir(component), mf_install_mode_to_string(mode));
        n_marked++;
    }
    if (n_marked == 0) {
        mfd_set_error_not_installed(error, ret_detail, component_id, version);
        return NULL;
    }

    /* list the marked versions again, to return them with their new install mode */
    g_clear_pointer(&components, g_ptr_array_unref);
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (mfd_manager_mark_selects(component, version, full))
            g_ptr_array_add(marked, g_object_ref(component));
    }

    return g_steal_pointer(&marked);
}

/**
 * mfd_manager_set_fetch_helper:
 * @helper_path: (nullable): path of a download helper that the manager starts
 *   itself, or %NULL to use the systemd service
 * @ca_file: (nullable): the only certificate authority file that a self-started
 *   download helper trusts
 *
 * Make the manager run the download helper itself. This is used by tests.
 */
void
mfd_manager_set_fetch_helper(MfdManager *manager, const char *helper_path, const char *ca_file)
{
    g_return_if_fail(MFD_IS_MANAGER(manager));

    g_free(manager->fetch_helper);
    manager->fetch_helper = g_strdup(helper_path);
    g_free(manager->fetch_ca_file);
    manager->fetch_ca_file = g_strdup(ca_file);
}

/**
 * mfd_manager_refresh:
 * @manager: an #MfdManager
 * @remote_ids: (nullable) (array zero-terminated=1): the remotes to refresh, or %NULL for all
 * @progress_func: (nullable): called whenever the operation progresses
 * @user_data: passed to @progress_func
 * @error: a #GError
 *
 * Download the metadata of remotes, and store it. If a remote fails, the
 * other remotes are still refreshed, and the error of the first failure is
 * returned. Refreshing all remotes also removes the cached data of remotes
 * that are not configured anymore.
 *
 * Returns: (transfer full): the result with the warnings of the refresh, or %NULL on error
 */
MfResult *
mfd_manager_refresh(MfdManager *manager,
                    const char *const *remote_ids,
                    MfProgressFunc progress_func,
                    gpointer user_data,
                    GError **error)
{
    g_auto(MfdProgressTracker) tracker = {
        .func = progress_func,
        .user_data = user_data,
        .progress = mf_progress_new(),
    };
    g_autoptr(MfResult) result = mf_result_new();
    g_autoptr(GPtrArray) remotes = NULL;
    g_autoptr(MfdFetcher) fetcher = NULL;
    g_autoptr(GError) first_error = NULL;
    const char *root;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    root = mf_store_get_path(manager->store);
    if (remote_ids == NULL) {
        remotes = mf_remote_list_all(error);
        if (remotes == NULL)
            return NULL;
    } else {
        remotes = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; remote_ids[i] != NULL; i++) {
            MfRemote *remote = mf_remote_new_for_id(remote_ids[i], error);

            if (remote == NULL)
                return NULL;
            g_ptr_array_add(remotes, remote);
        }
    }

    fetcher = mfd_fetcher_new(manager->fetch_helper, manager->fetch_ca_file);

    tracker.total = remotes->len;
    for (guint i = 0; i < remotes->len; i++) {
        MfRemote *remote = g_ptr_array_index(remotes, i);
        g_autoptr(GError) local_error = NULL;

        mfd_progress_begin_item(&tracker, MF_ACTION_REFRESH, mf_remote_get_id(remote), NULL, 0);
        if (!mfd_refresh_remote(root, fetcher, remote, result, NULL, &local_error)) {
            g_warning("Unable to refresh %s: %s", mf_remote_get_id(remote), local_error->message);
            if (first_error == NULL)
                first_error = g_steal_pointer(&local_error);
        }
        mfd_progress_end_item(&tracker);
    }

    if (remote_ids == NULL && !mfd_refresh_remove_unconfigured(root, remotes, error))
        return NULL;
    if (first_error != NULL) {
        g_propagate_error(error, g_steal_pointer(&first_error));
        return NULL;
    }

    return g_steal_pointer(&result);
}
