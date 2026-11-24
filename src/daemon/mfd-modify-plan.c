/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-modify-plan.h"

#include <inttypes.h>
#include <sys/stat.h>

#include "mf-component-private.h"
#include "mf-manifest-private.h"
#include "mf-store-private.h"
#include "mf-utils-private.h"

#include "mfd-utils.h"

/*
 * Plans for changes to the store.
 *
 * An install plan says how one item of an install set will be installed, and
 * a removal plan lists the components and images that a removal deletes.
 * Creating a plan checks whether the operation can be done, and does not
 * change the store. The #MfdManager executes the plans.
 */

/**
 * mfd_install_plan_clear:
 */
static void
mfd_install_plan_clear(MfdInstallPlan *iplan)
{
    g_clear_pointer(&iplan->base, mf_base_link_free);
    g_clear_pointer(&iplan->base_sdk, mf_base_link_free);
    g_clear_object(&iplan->previous);
}

/**
 * mfd_install_plan_compare_layer:
 *
 * Order plans by the layer of their component, from the bottom to the top,
 * so every component comes after the components that it builds on.
 */
static int
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
 * mfd_manifest_becomes_current:
 *
 * Returns: %TRUE unless a newer version of the component is already installed
 */
static gboolean
mfd_manifest_becomes_current(MfStore *store, MfManifest *manifest)
{
    g_autoptr(MfComponent)
        newest = mf_store_get_component(store, mf_manifest_get_component_id(manifest), NULL, NULL);

    return newest == NULL || mf_version_compare_with_rev(mf_manifest_get_version(manifest),
                                                         mf_manifest_get_revision(manifest),
                                                         mf_component_get_version(newest),
                                                         mf_component_get_revision(newest)) >= 0;
}

/**
 * mfd_find_other_kind:
 *
 * An ID names exactly one kind of component.
 * This method searches the installed components for ones that
 * have the same ID as @manifest, but have a different kind.
 *
 * Returns: (transfer full) (nullable): an installed version with the same ID as @manifest, but of a different kind
 */
static MfComponent *
mfd_find_other_kind(MfStore *store, MfManifest *manifest)
{
    g_autoptr(GPtrArray) versions = NULL;

    versions = mf_store_list_components(store,
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
 * mfd_plan_base_link:
 * @component_id: the component that needs the referenced component
 * @ref: a reference to a base or to a base SDK
 * @ret_manifest: (out) (transfer full): the manifest of the component that the link points to
 *
 * Find the component that is used for a base reference, in the set or in
 * the store, and create the link to it.
 *
 * If the reference is not exact and the found version is the current version
 * once the set is installed, the link points to `current`. Otherwise, the
 * link points to the found version.
 *
 * Returns: (transfer full) (nullable): the link, or %NULL with @error set if
 *   no component for @ref is in the set or in the store
 */
static MfBaseLink *
mfd_plan_base_link(MfStore *store,
                   MfdInstallSet *set,
                   const char *component_id,
                   const MfBaseReference *ref,
                   MfManifest **ret_manifest,
                   MfdErrorDetail *ret_detail,
                   GError **error)
{
    g_auto(MfdBaseProvider) provider = {};
    g_autoptr(MfManifest) manifest = NULL;
    g_autofree char *version_name = NULL;
    gboolean current;

    if (!mfd_install_set_find_base(set, store, ref, &provider)) {
        mfd_set_error_missing_base(error, ret_detail, component_id, ref);
        return NULL;
    }

    if (provider.item != NULL) {
        manifest = g_object_ref(provider.item->manifest);
        current = mfd_manifest_becomes_current(store, manifest);
    } else {
        manifest = mf_component_fetch_manifest(provider.installed, error);
        if (manifest == NULL)
            return NULL;
        current = mf_component_is_current(provider.installed);
    }

    if (ref->exact || !current)
        version_name = mf_build_full_version(mf_manifest_get_version(manifest),
                                             mf_manifest_get_revision(manifest));
    *ret_manifest = g_object_ref(manifest);
    return mf_base_link_new(
        mf_store_subdir_for_kind(mf_manifest_get_kind(manifest), mf_manifest_is_sdk(manifest)),
        mf_manifest_get_component_id(manifest),
        version_name);
}

/**
 * mfd_install_set_needs_exact_version:
 *
 * Returns: %TRUE if an item of the set requires the exact version of @manifest
 *   as its base or as its base SDK
 */
static gboolean
mfd_install_set_needs_exact_version(MfdInstallSet *set, MfManifest *manifest)
{
    for (guint i = 0; i < set->items->len; i++) {
        MfManifest *other = ((MfdInstallItem *) g_ptr_array_index(set->items, i))->manifest;
        const MfBaseReference *base = mf_manifest_get_base(other);

        if (base != NULL && base->exact && mf_base_reference_matches_version(base, manifest))
            return TRUE;
        if (mf_base_reference_matches_version(mf_manifest_get_base_sdk(other), manifest))
            return TRUE;
    }

    return FALSE;
}

/**
 * mfd_manifests_have_same_base:
 *
 * Returns: %TRUE if both manifests reference the same version and revision as their base
 */
static gboolean
mfd_manifests_have_same_base(MfManifest *a, MfManifest *b)
{
    const MfBaseReference *base_a = mf_manifest_get_base(a);
    const MfBaseReference *base_b = mf_manifest_get_base(b);

    return base_a != NULL && base_b != NULL && g_str_equal(base_a->id, base_b->id) &&
           g_str_equal(base_a->version, base_b->version) && base_a->revision == base_b->revision;
}

/**
 * mfd_install_plan_init_base_sdk:
 * @iplan: the plan to set the base SDK link for
 * @item: an item that has a base SDK
 * @base: the manifest of the base of @item
 *
 * Find the base SDK of an item in the set or in the store, and set the
 * base SDK link of @iplan.
 *
 * The versions of an SDK stack are locked together: the base SDK must extend
 * the exact version that the base of @item was built on. If the base of
 * @item was built on an SDK directly, that SDK must be the base SDK.
 *
 * Returns: %TRUE if the base SDK was found and is valid
 */
static gboolean
mfd_install_plan_init_base_sdk(MfdInstallPlan *iplan,
                               MfStore *store,
                               MfdInstallSet *set,
                               MfdInstallItem *item,
                               MfManifest *base,
                               MfdErrorDetail *ret_detail,
                               GError **error)
{
    MfManifest *manifest = item->manifest;
    const char *component_id = mf_manifest_get_component_id(manifest);
    const MfBaseReference *ref = mf_manifest_get_base_sdk(manifest);
    const MfBaseReference *base_ref = mf_manifest_get_base(base);
    g_autoptr(MfManifest) base_sdk = NULL;

    iplan->base_sdk = mfd_plan_base_link(store, set, component_id, ref, &base_sdk, ret_detail, error);
    if (iplan->base_sdk == NULL)
        return FALSE;

    if (!mf_manifest_is_sdk(base_sdk)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    "%s: %s has the base SDK %s, which is not an SDK",
                    item->name,
                    component_id,
                    ref->id);
        return FALSE;
    }
    /* the base SDK is the SDK of the layer below the base, and a core has no layer below it */
    if (base_ref == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    "%s: %s has the base SDK %s, but its base %s is a core",
                    item->name,
                    component_id,
                    ref->id,
                    mf_manifest_get_component_id(base));
        return FALSE;
    }
    if (!mfd_manifests_have_same_base(base_sdk, base) &&
        !mf_base_reference_matches_version(base_ref, base_sdk)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    "%s: the base SDK %s %s-%" PRIu64 " is not the SDK of %s %s-%" PRIu64
                    ", the base of %s %s-%" PRIu64,
                    item->name,
                    ref->id,
                    ref->version,
                    ref->revision,
                    base_ref->id,
                    base_ref->version,
                    base_ref->revision,
                    mf_manifest_get_component_id(base),
                    mf_manifest_get_version(base),
                    mf_manifest_get_revision(base));
        return FALSE;
    }

    return TRUE;
}

/**
 * mfd_install_plan_init:
 *
 * Plan the installation of an item of the set.
 *
 * The plan contains info on whether the image is added to the store, whether the item is
 * skipped because the component is installed already or is not needed, which install-mode
 * the component gets, and which base and base SDK it links to.
 *
 * Returns: %TRUE if the item can be installed
 */
static gboolean
mfd_install_plan_init(MfdInstallPlan *iplan,
                      MfStore *store,
                      MfdInstallSet *set,
                      MfdInstallItem *item,
                      MfdErrorDetail *ret_detail,
                      GError **error)
{
    MfManifest *manifest = item->manifest;
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *version = mf_manifest_get_version(manifest);
    const char *verity_hash = mf_manifest_get_verity_hash(manifest);
    const MfBaseReference *base_ref = mf_manifest_get_base(manifest);
    MfComponentKind kind = mf_manifest_get_kind(manifest);
    gboolean sdk = mf_manifest_is_sdk(manifest);
    gboolean dependency = item->is_dependency;
    g_autoptr(MfComponent) other_kind = NULL;
    g_autoptr(MfComponent) existing = NULL;
    g_autoptr(MfComponent) newest = NULL;

    if (!g_str_equal(mf_manifest_get_architecture(manifest), mf_get_host_architecture())) {
        mfd_set_error_wrong_architecture(error,
                                         ret_detail,
                                         component_id,
                                         mf_manifest_get_architecture(manifest));
        return FALSE;
    }
    other_kind = mfd_find_other_kind(store, manifest);
    if (other_kind != NULL) {
        mfd_set_error_id_conflict(error, ret_detail, other_kind);
        return FALSE;
    }
    if (!mf_manifest_check_integration(manifest, error))
        return FALSE;
    if (!mf_manifest_check_permissions(manifest, error))
        return FALSE;

    iplan->item = item;
    iplan->add_image = !mf_store_has_image(store, verity_hash);
    iplan->mode = dependency || mf_component_kind_is_always_auto(kind, sdk) ? MF_INSTALL_MODE_AUTO
                                                                            : MF_INSTALL_MODE_MANUAL;

    existing = mf_store_find_version(store, component_id, version, mf_manifest_get_revision(manifest), NULL);
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

    newest = mf_store_get_component(store, component_id, NULL, NULL);
    if (newest != NULL && mf_version_compare_with_rev(mf_component_get_version(newest),
                                                      mf_component_get_revision(newest),
                                                      version,
                                                      mf_manifest_get_revision(manifest)) < 0)
        iplan->previous = g_steal_pointer(&newest);

    /* a dependency is not needed if the same or a newer version of the component is installed
     * and provides the same identity. A dependency that another item requires in its exact
     * version is always installed, as a different version can not replace it. */
    if (dependency && !mfd_install_set_needs_exact_version(set, manifest)) {
        g_autoptr(MfComponent)
            provider = mf_store_find_identity(store, mf_manifest_get_identity(manifest), NULL);

        if (!mfd_check_if_pending_item_wins(manifest, provider)) {
            g_info("%s %s is not needed, %s is installed",
                   component_id,
                   version,
                   mf_component_get_full_version(provider));
            iplan->skip = TRUE;
            iplan->add_image = FALSE;
            return TRUE;
        }
    }

    if (base_ref != NULL) {
        g_autoptr(MfManifest) base = NULL;
        MfComponentKind base_kind;
        gboolean base_sdk;

        iplan->base = mfd_plan_base_link(store, set, component_id, base_ref, &base, ret_detail, error);
        if (iplan->base == NULL)
            return FALSE;

        /* a component can only build on the kinds of layer that are allowed below it */
        base_kind = mf_manifest_get_kind(base);
        base_sdk = mf_manifest_is_sdk(base);
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

        /* a component with a base SDK is always an SDK, so its exact base was found above */
        if (mf_manifest_get_base_sdk(manifest) != NULL &&
            !mfd_install_plan_init_base_sdk(iplan, store, set, item, base, ret_detail, error))
            return FALSE;
    }

    return TRUE;
}

/**
 * mfd_install_plans_check_file_conflicts:
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
mfd_install_plans_check_file_conflicts(GArray *plans,
                                       MfStore *store,
                                       MfdIntegrator *integrator,
                                       GError **error)
{
    g_autoptr(GPtrArray) manifests = g_ptr_array_sized_new(plans->len);

    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        MfManifest *manifest = plan->item->manifest;

        if (!plan->skip && mfd_manifest_becomes_current(store, manifest))
            g_ptr_array_add(manifests, manifest);
    }

    return mfd_integrator_check_conflicts(integrator, manifests, error);
}

/**
 * mfd_install_plans_new_for_set:
 * @store: the store that the set will be installed into
 * @integrator: the integrator of @store, to check for file conflicts on the host
 * @set: the components to install
 * @ret_detail: (out caller-allocates) (optional): details about the failure
 *
 * Plan the installation of every item of a set.
 *
 * Returns: (transfer full) (element-type MfdInstallPlan): the plans, or %NULL
 *   if the set can not be installed
 */
GArray *
mfd_install_plans_new_for_set(MfStore *store,
                              MfdIntegrator *integrator,
                              MfdInstallSet *set,
                              MfdErrorDetail *ret_detail,
                              GError **error)
{
    GPtrArray *items = set->items;
    g_autoptr(GArray) plans = g_array_sized_new(FALSE, TRUE, sizeof(MfdInstallPlan), items->len);

    g_array_set_size(plans, items->len);
    g_array_set_clear_func(plans, (GDestroyNotify) mfd_install_plan_clear);
    for (guint i = 0; i < items->len; i++) {
        if (!mfd_install_plan_init(&g_array_index(plans, MfdInstallPlan, i),
                                   store,
                                   set,
                                   g_ptr_array_index(items, i),
                                   ret_detail,
                                   error))
            return NULL;
    }
    if (!mfd_install_plans_check_file_conflicts(plans, store, integrator, error))
        return NULL;

    /* install every component after the components that it builds on */
    g_array_sort(plans, mfd_install_plan_compare_layer);

    return g_steal_pointer(&plans);
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
 *
 * Returns: (transfer none): the new item of the plan
 */
static MfdRemovalItem *
mfd_removal_plan_add_item(MfdRemovalPlan *rmplan, MfComponent *component, MfAction action)
{
    MfdRemovalItem *item = g_new0(MfdRemovalItem, 1);

    item->component = g_object_ref(component);
    item->action = action;
    g_ptr_array_add(rmplan->items, item);
    g_hash_table_add(rmplan->cpts_set, item->component);

    return item;
}

/**
 * mfd_find_used_bases:
 * @installed: (element-type MfComponent): every installed version
 * @rm_pending: (element-type MfComponent): versions that are planned for removal
 *
 * Find the versions that are used as a base or as a base SDK by an installed
 * version which is not in @rm_pending.
 *
 * A link to `current` counts as a link to the current version. A base is not
 * counted as used if its link can not be read, or if it is not installed.
 *
 * Returns: a table that maps every used version to one of the versions that link to it
 */
static GHashTable *
mfd_find_used_bases(MfStore *store, GPtrArray *installed, GHashTable *rm_pending)
{
    GHashTable *used = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, NULL);

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);
        MfComponent *base;
        MfComponent *base_sdk;

        if (g_hash_table_contains(rm_pending, component))
            continue;
        base = mf_store_get_base(store, component, NULL);
        if (base != NULL)
            g_hash_table_insert(used, base, component);
        base_sdk = mf_store_get_base_sdk(store, component, NULL);
        if (base_sdk != NULL)
            g_hash_table_insert(used, base_sdk, component);
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
 * mfd_removal_plan_complete:
 * @rmplan: the removal plan, which may already list versions planned for removal
 * @installed: (element-type MfComponent): every installed version
 * @requested: (element-type MfComponent): the versions that were requested to be removed
 *
 * Add the versions and images that have to be removed to the plan, in the
 * order of their removal.
 *
 * A version is planned for removal if its removal was requested, or if it is
 * marked as auto and is not the newest version of an application or plugin.
 * In both cases, the version is only planned for removal if no kept version
 * links to it. The search is repeated until no more versions are found, as
 * removing a version can leave its base unused. Every image that no kept
 * version uses is planned for removal as well.
 */
static gboolean
mfd_removal_plan_complete(MfdRemovalPlan *rmplan,
                          MfStore *store,
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
        g_autoptr(GHashTable) used = mfd_find_used_bases(store, installed, rmplan->cpts_set);

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
    images = mf_store_list_images(store, error);
    if (images == NULL)
        return FALSE;
    for (guint i = 0; i < images->len; i++) {
        const char *hash = g_ptr_array_index(images, i);
        g_autofree char *path = NULL;
        struct stat st;

        if (g_hash_table_contains(images_used, hash))
            continue;
        path = mf_store_image_path_at(mf_store_get_path(store), hash);
        if (stat(path, &st) == 0)
            rmplan->image_bytes += (uint64_t) st.st_size;
        g_ptr_array_add(rmplan->images, g_strdup(hash));
    }

    return TRUE;
}

/**
 * mfd_removal_plan_new_for_removal:
 * @store: an #MfStore
 * @component_id: (nullable): a component to remove, or %NULL to only plan a cleanup
 * @version: (nullable): the version to remove with all of its revisions, the
 *   full version (`<version>-<rev>`) to remove one revision, or %NULL for all versions
 * @ret_detail: (out caller-allocates) (optional): details about the failure
 * @error: a #GError
 *
 * Create the removal plan for a component. The plan contains the requested
 * versions of the component, every dependency that no kept component builds
 * on after their removal, and every image that no kept component uses.
 *
 * Returns: (transfer full): the plan, or %NULL on error
 */
MfdRemovalPlan *
mfd_removal_plan_new_for_removal(MfStore *store,
                                 const char *component_id,
                                 const char *version,
                                 MfdErrorDetail *ret_detail,
                                 GError **error)
{
    g_autoptr(MfdRemovalPlan) rmplan = mfd_removal_plan_new();
    g_autoptr(GPtrArray) installed = NULL;
    g_autoptr(GHashTable) requested = g_hash_table_new(g_direct_hash, g_direct_equal);
    gboolean full = FALSE;

    g_return_val_if_fail(MF_IS_STORE(store), NULL);

    installed = mf_store_list_components(store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
    if (installed == NULL)
        return NULL;

    if (component_id != NULL) {
        if (!mfd_store_check_version_is_full(store, component_id, version, &full, error))
            return NULL;
        for (guint i = 0; i < installed->len; i++) {
            MfComponent *candidate = g_ptr_array_index(installed, i);

            if (g_str_equal(mf_component_get_id(candidate), component_id) &&
                (version == NULL || mfd_component_matches_version(candidate, version, full)))
                g_hash_table_add(requested, candidate);
        }
        if (g_hash_table_size(requested) == 0) {
            mfd_set_error_not_installed(error, ret_detail, component_id, version);
            return NULL;
        }
    }

    if (!mfd_removal_plan_complete(rmplan, store, installed, requested, error))
        return NULL;

    /* a requested version that is not planned for removal is still linked to by a kept version */
    for (guint i = 0; i < installed->len; i++) {
        MfComponent *candidate = g_ptr_array_index(installed, i);
        g_autoptr(GHashTable) used = NULL;

        if (!g_hash_table_contains(requested, candidate) ||
            g_hash_table_contains(rmplan->cpts_set, candidate))
            continue;
        used = mfd_find_used_bases(store, installed, rmplan->cpts_set);
        mfd_set_error_in_use(error,
                             ret_detail,
                             component_id,
                             mf_component_get_id(g_hash_table_lookup(used, candidate)));
        return NULL;
    }

    return g_steal_pointer(&rmplan);
}

/**
 * mfd_removal_plan_new_for_superseded:
 * @plans: (element-type MfdInstallPlan): the plans of the installation
 *
 * Create the removal plan for the versions that an installation supersedes.
 *
 * A superseded version is an older version or revision of a component that
 * was installed, which is marked as auto and which no kept version links to.
 * Dependencies and images that are unused afterwards are added to the plan,
 * like in mfd_removal_plan_new_for_removal().
 *
 * Returns: (transfer full): the plan, or %NULL on error
 */
MfdRemovalPlan *
mfd_removal_plan_new_for_superseded(MfStore *store, GArray *plans, GError **error)
{
    g_autoptr(MfdRemovalPlan) rmplan = mfd_removal_plan_new();
    g_autoptr(GPtrArray) installed = NULL;
    g_autoptr(GHashTable) superseded = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_autoptr(GHashTable) used = NULL;
    gboolean kept;

    installed = mf_store_list_components(store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
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
        used = mfd_find_used_bases(store, installed, superseded);
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
            mfd_removal_plan_add_item(rmplan, component, MF_ACTION_CLEANUP)->superseded = TRUE;
    }

    /* if no version is superseded, we do not clean up anything else either */
    if (rmplan->items->len > 0 && !mfd_removal_plan_complete(rmplan, store, installed, superseded, error))
        return NULL;
    return g_steal_pointer(&rmplan);
}
