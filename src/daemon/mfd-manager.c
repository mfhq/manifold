/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-manager.h"

#include <appstream.h>

#include "mf-component-private.h"
#include "mf-integration-private.h"
#include "mf-manifest-private.h"
#include "mf-result-private.h"
#include "mf-store-private.h"
#include "mf-progress-private.h"
#include "mf-utils-private.h"

#include "mfd-image-reader.h"
#include "mfd-install-set.h"
#include "mfd-integrator.h"
#include "mfd-modify-plan.h"
#include "mfd-progress-tracker.h"
#include "mfd-remote-channel.h"
#include "mfd-refresh.h"
#include "mfd-resolver.h"
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

    return write->item->write_image(write->item, fd, mfd_progress_on_copy, write->tracker, error);
}

/**
 * mfd_manager_execute_removal:
 * @result: the result to add the removed components and images to
 *
 * Execute a removal plan: remove the version directories first, then the images.
 *
 * Progress is reported for the components only. A superseded version is
 * reported as part of the update that replaced it (see #MfUpdate), so no
 * progress item is reported for it, and it is not added to the removed
 * components of @result.
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

        if (item->superseded) {
            if (!mfd_store_writer_remove_component(manager->writer, item->component, error))
                return FALSE;
            mfd_progress_skip_item(tracker);
            continue;
        }

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
    char *xml;

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
 * mfd_manager_read_checked_exports:
 * @manifest: the manifest of an image in the store
 *
 * Read the integration files of an image, and check that they can be
 * installed on the host.
 *
 * Returns: (transfer full) (element-type utf8 GBytes): the contents of the
 *   files by their export path, or %NULL on error
 */
static GHashTable *
mfd_manager_read_checked_exports(MfdManager *manager, MfManifest *manifest, GError **error)
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
                                 uint64_t *ret_bytes_added,
                                 GError **error)
{
    MfdInstallItem *item = iplan->item;
    MfManifest *manifest = item->manifest;
    const char *verity_hash = mf_manifest_get_verity_hash(manifest);
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *version = mf_manifest_get_version(manifest);
    g_autofree char *full_version = mf_build_full_version(version, mf_manifest_get_revision(manifest));
    uint64_t image_size = mf_manifest_get_image_size(manifest);
    g_autoptr(GBytes) catalog = NULL;
    g_autoptr(GHashTable) exports = NULL;
    g_autoptr(GError) local_error = NULL;
    gboolean added = FALSE;

    *ret_bytes_added = 0;
    /* build the catalog data for the local origin first, as that can fail */
    if (item->catalog != NULL) {
        catalog = mfd_make_local_appstream_catalog(item->catalog, error);
        if (catalog == NULL) {
            g_prefix_error(error, "%s: ", component_id);
            return FALSE;
        }
    }

    if (iplan->add_image)
        mfd_progress_begin_item(tracker,
                                iplan->previous != NULL ? MF_ACTION_UPDATE : MF_ACTION_INSTALL,
                                component_id,
                                full_version,
                                image_size);

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
    exports = mfd_manager_read_checked_exports(manager, manifest, &local_error);
    if (exports == NULL || !mfd_store_writer_install_component(manager->writer,
                                                               manifest,
                                                               iplan->base,
                                                               iplan->base_sdk,
                                                               catalog,
                                                               item->icons,
                                                               exports,
                                                               item->origin,
                                                               iplan->mode,
                                                               &local_error)) {
        g_autoptr(GError) remove_error = NULL;

        /* the same version was installed in the meantime, and it uses the image, so we keep it */
        if (g_error_matches(local_error, MF_ERROR, MF_ERROR_ALREADY_INSTALLED)) {
            mfd_set_error_already_installed(error, component_id, version);
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
 * mfd_manager_results_add_installed:
 * @result: the result to add the installed components to
 * @plans: (element-type MfdInstallPlan): the plans of the installation
 *
 * Add the components that were installed from the set to @result.
 * Dependencies that were installed already are not added. If an older
 * version of a component was installed, the component is added as an update.
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
        component = mf_store_find_version(manager->store,
                                          mf_manifest_get_component_id(manifest),
                                          mf_manifest_get_version(manifest),
                                          mf_manifest_get_revision(manifest),
                                          NULL);
        if (component == NULL ||
            g_strcmp0(mf_component_get_verity_hash(component), mf_manifest_get_verity_hash(manifest)) != 0)
            continue;
        if (plan->previous == NULL) {
            mf_result_add_installed(result, component);
            continue;
        }

        g_autoptr(MfUpdate) update = mf_update_new(component, mf_component_get_full_version(plan->previous));
        g_autoptr(MfManifest) previous_manifest = mf_component_fetch_manifest(plan->previous, NULL);

        /* NOTE: The previous version may be removed already, but its manifest was loaded together
         * with the store, so we can still read its integration. */
        mf_integration_diff_privileged(
            previous_manifest != NULL ? mf_manifest_get_integration(previous_manifest) : NULL,
            mf_manifest_get_integration(manifest),
            mf_update_get_integration_added(update),
            mf_update_get_integration_removed(update));
        mf_result_add_updated(result, update);
    }
}

/**
 * mfd_manager_install_set_gains_privileges:
 * @manager: an #MfdManager
 * @set: the install set of an update
 *
 * Check whether installing a set makes a component privileged that was not
 * privileged before, which gives the component root access to the host.
 * This is the case if a component of the set is privileged, and its newest
 * installed version is not privileged or no version of it is installed.
 *
 * NOTE: We do not compare the integration files themselves. An admin approved
 * the component when it was installed, and with that trusts its origin to
 * update it, even if the update changes the integration files.
 * The changes are reported with the update, see #MfUpdate.
 *
 * Returns: %TRUE if a component becomes privileged
 */
gboolean
mfd_manager_install_set_gains_privileges(MfdManager *manager, MfdInstallSet *set)
{
    g_return_val_if_fail(MFD_IS_MANAGER(manager), TRUE);

    for (guint i = 0; i < set->items->len; i++) {
        MfdInstallItem *item = g_ptr_array_index(set->items, i);
        g_autoptr(MfComponent) newest = NULL;
        g_autoptr(MfManifest) manifest = NULL;

        if (!mf_manifest_is_privileged(item->manifest))
            continue;

        newest = mf_store_get_component(manager->store,
                                        mf_manifest_get_component_id(item->manifest),
                                        NULL,
                                        NULL);
        manifest = newest != NULL ? mf_component_fetch_manifest(newest, NULL) : NULL;
        if (manifest == NULL || !mf_manifest_is_privileged(manifest))
            return TRUE;
    }

    return FALSE;
}

/**
 * mfd_manager_install_set:
 * @manager: an #MfdManager
 * @set: the components to install
 * @progress_func: (nullable): called whenever the operation progresses
 * @user_data: passed to @progress_func
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
    uint64_t bytes_added = 0;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    if (!mfd_store_writer_ensure_root(manager->writer, error))
        return NULL;
    /* reload the store, to ensure all our information is up-to-date */
    if (!mf_store_load(manager->store, error))
        return NULL;

    mfd_integrator_reconcile_all(manager->integrator);

    /* plan installation */
    plans = mfd_install_plans_new_for_set(manager->store, manager->integrator, set, error);
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
    for (guint i = 0; i < set->warnings->len; i++)
        mfd_emit_warning(result, "%s", (const char *) g_ptr_array_index(set->warnings, i));
    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        uint64_t image_bytes;

        if (plan->skip)
            continue;
        if (!mfd_manager_execute_install_plan(manager, plan, &tracker, result, &image_bytes, error))
            return NULL;
        bytes_added += image_bytes;
    }

    /* remove the superseded versions. The installation is still successful if their removal fails. */
    rmplan = mfd_removal_plan_new_for_superseded(manager->store, plans, &supersede_error);
    /* the number of versions to remove is only known now */
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
    rmplan = mfd_removal_plan_new_for_removal(manager->store, component_id, version, error);
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
 * mfd_component_is_mark_target:
 *
 * Check whether @component is one of the versions to mark. If @version is
 * %NULL, only the current version is marked.
 */
static gboolean
mfd_component_is_mark_target(MfComponent *component, const char *version, gboolean full)
{
    if (version == NULL)
        return mf_component_is_current(component);
    return mfd_component_matches_version(component, version, full);
}

/**
 * mfd_manager_mark:
 * @manager: an #MfdManager
 * @component_id: the component to mark
 * @version: (nullable): the version to mark with all of its revisions, the
 *   full version (`<version>-<rev>`) to mark one revision, or %NULL for the current version
 * @mode: the install mode to record
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
    if (!mfd_store_check_version_is_full(manager->store, component_id, version, &full, error))
        return NULL;
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (!mfd_component_is_mark_target(component, version, full))
            continue;
        if (!mfd_store_writer_set_mode(manager->writer, component, mode, error))
            return NULL;
        g_message("Marked %s as %s", mf_component_get_dir(component), mf_install_mode_to_string(mode));
        n_marked++;
    }
    if (n_marked == 0) {
        mfd_set_error_not_installed(error, component_id, version);
        return NULL;
    }

    /* list the marked versions again, to return them with their new install mode */
    g_clear_pointer(&components, g_ptr_array_unref);
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (mfd_component_is_mark_target(component, version, full))
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
 * mfd_manager_new_fetcher:
 *
 * Returns: (transfer full): a fetcher for the download helper
 */
static MfdFetcher *
mfd_manager_new_fetcher(MfdManager *manager)
{
    return mfd_fetcher_new(manager->fetch_helper, manager->fetch_ca_file);
}

/**
 * mfd_manager_refresh_for_retry:
 * @manager: an #MfdManager
 * @request: the request that failed
 * @set: the set that was resolved or installed for @request
 * @failure: the error that @request failed with
 *
 * Refresh the remotes that a failed request downloaded from, if that may fix
 * the failure.
 *
 * The files of a repository change when it publishes a new root index. The
 * files we download then do not match the cached metadata anymore, and the
 * verification or the download fails. In this case we refresh the remotes,
 * and the caller tries the request again. We do this only once for a request.
 *
 * Returns: %TRUE if the caller should try again, because a remote has a new
 *   root index now
 */
gboolean
mfd_manager_refresh_for_retry(MfdManager *manager,
                              MfdRepoRequest *request,
                              MfdInstallSet *set,
                              const GError *failure)
{
    g_autoptr(MfdFetcher) fetcher = NULL;
    gboolean changed = FALSE;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), FALSE);

    if (request->refreshed || set->remotes->len == 0 ||
        !(g_error_matches(failure, MF_ERROR, MF_ERROR_VERIFICATION_FAILED) ||
          g_error_matches(failure, MF_ERROR, MF_ERROR_DOWNLOAD_FAILED)))
        return FALSE;
    request->refreshed = TRUE;

    fetcher = mfd_manager_new_fetcher(manager);
    for (guint i = 0; i < set->remotes->len; i++) {
        MfRemote *remote = g_ptr_array_index(set->remotes, i);
        g_autoptr(GError) local_error = NULL;
        gboolean remote_changed = FALSE;

        g_message("Refreshing %s, as its files may have changed: %s",
                  mf_remote_get_id(remote),
                  failure->message);
        if (!mfd_refresh_remote(mf_store_get_path(manager->store),
                                fetcher,
                                remote,
                                NULL,
                                &remote_changed,
                                &local_error))
            g_warning("Unable to refresh %s: %s", mf_remote_get_id(remote), local_error->message);
        changed |= remote_changed;
    }

    return changed;
}

/**
 * mfd_manager_resolve_request:
 * @manager: an #MfdManager
 * @request: the request to resolve
 * @error: a #GError
 *
 * Find the versions that a request installs in the channels of the remotes,
 * together with the bases they need that are not installed, and create an
 * install set from them. We download the manifests, but not the images yet.
 * A component that an update request can not update is left out, and the
 * set has a warning about it.
 *
 * Returns: (transfer full): the set, which may have no items for an update
 *   request, or %NULL on error
 */
MfdInstallSet *
mfd_manager_resolve_request(MfdManager *manager, MfdRepoRequest *request, GError **error)
{
    g_autoptr(MfdFetcher) fetcher = NULL;
    const char *root;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    if (!mfd_store_writer_ensure_root(manager->writer, error))
        return NULL;
    if (!mf_store_load(manager->store, error))
        return NULL;
    root = mf_store_get_path(manager->store);
    fetcher = mfd_manager_new_fetcher(manager);

    for (;;) {
        g_autoptr(MfdRemoteChannelSet) channel_set = mfd_remote_channel_set_load(root, error);
        g_autoptr(GPtrArray) failures = g_ptr_array_new_with_free_func((GDestroyNotify) g_error_free);
        g_autoptr(MfdInstallSet) set = NULL;
        g_autoptr(GError) local_error = NULL;
        gboolean retry = FALSE;

        if (channel_set == NULL)
            return NULL;

        set = mfd_install_set_new();
        if (!mfd_resolver_resolve(request,
                                  set,
                                  manager->store,
                                  fetcher,
                                  channel_set,
                                  failures,
                                  &local_error)) {
            if (mfd_manager_refresh_for_retry(manager, request, set, local_error))
                continue;
            g_propagate_error(error, g_steal_pointer(&local_error));
            return NULL;
        }

        /* a refresh may fix an update that could not be resolved */
        for (guint i = 0; i < failures->len && !retry; i++)
            retry = mfd_manager_refresh_for_retry(manager, request, set, g_ptr_array_index(failures, i));
        if (retry)
            continue;

        for (guint i = 0; i < failures->len; i++)
            g_ptr_array_add(set->warnings, g_strdup(((GError *) g_ptr_array_index(failures, i))->message));
        return g_steal_pointer(&set);
    }
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

    fetcher = mfd_manager_new_fetcher(manager);

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

/**
 * mfd_manager_inspect_remote:
 * @manager: an #MfdManager
 * @url: the https URL of a repository
 * @public_key: (nullable): the public key that has to sign the repository, or
 *   %NULL to use the key the repository serves
 * @error: a #GError
 *
 * Download and verify the root index of a repository, before it is added as a remote.
 *
 * Returns: (transfer full): the repository, or %NULL on error
 */
MfdRemoteInspection *
mfd_manager_inspect_remote(MfdManager *manager, const char *url, const char *public_key, GError **error)
{
    g_autoptr(MfdFetcher) fetcher = NULL;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    fetcher = mfd_manager_new_fetcher(manager);
    return mfd_remotes_inspect(fetcher, url, public_key, NULL, error);
}

/**
 * mfd_manager_add_remote:
 * @manager: an #MfdManager
 * @remote_id: the ID of the repository, which the user confirmed
 * @url: the https URL of a repository
 * @public_key: the public key the user decided to trust for the repository
 * @channels: (nullable): the channels to install from, or %NULL for only `main`
 * @progress_func: (nullable): called whenever the operation progresses
 * @user_data: passed to @progress_func
 * @error: a #GError
 *
 * Add a repository as a remote that trusts @public_key, and refresh it. If
 * the refresh fails, the remote stays, because its root index and its key
 * were verified already. The result then has a warning with the reason.
 *
 * Returns: (transfer full): the result with the warnings of the refresh, or %NULL on error
 */
MfResult *
mfd_manager_add_remote(MfdManager *manager,
                       const char *remote_id,
                       const char *url,
                       const char *public_key,
                       const char *const *channels,
                       MfProgressFunc progress_func,
                       gpointer user_data,
                       GError **error)
{
    g_auto(MfdProgressTracker) tracker = {
        .func = progress_func,
        .user_data = user_data,
        .progress = mf_progress_new(),
        .total = 1,
    };
    g_autoptr(MfResult) result = mf_result_new();
    g_autoptr(MfdFetcher) fetcher = NULL;
    g_autoptr(MfdRemoteInspection) inspection = NULL;
    g_autoptr(MfRemote) remote = NULL;
    g_autoptr(GError) local_error = NULL;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    /* we inspect the repository again, with exactly the ID and key that the user confirmed */
    fetcher = mfd_manager_new_fetcher(manager);
    inspection = mfd_remotes_inspect(fetcher, url, public_key, remote_id, error);
    if (inspection == NULL)
        return NULL;
    remote = mfd_remotes_add(inspection, channels, error);
    if (remote == NULL)
        return NULL;

    mfd_progress_begin_item(&tracker, MF_ACTION_REFRESH, mf_remote_get_id(remote), NULL, 0);
    if (!mfd_refresh_remote(mf_store_get_path(manager->store), fetcher, remote, result, NULL, &local_error))
        mfd_emit_warning(result,
                         "The remote %s was added, but refreshing it failed: %s",
                         mf_remote_get_id(remote),
                         local_error->message);
    mfd_progress_end_item(&tracker);

    return g_steal_pointer(&result);
}

/**
 * mfd_manager_remove_remote:
 * @manager: an #MfdManager
 * @remote_id: the ID of a remote
 * @error: a #GError
 *
 * Remove a remote and its cached data.
 *
 * Returns: %TRUE if the remote was removed
 */
gboolean
mfd_manager_remove_remote(MfdManager *manager, const char *remote_id, GError **error)
{
    g_return_val_if_fail(MFD_IS_MANAGER(manager), FALSE);
    return mfd_remotes_remove(mf_store_get_path(manager->store), remote_id, error);
}
