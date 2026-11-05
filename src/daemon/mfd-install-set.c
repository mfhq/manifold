/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-install-set.h"

#include "mf-bundle-private.h"

/**
 * MfdBundleSource:
 *
 * A bundle source, containing the exact image that we
 * intend to copy.
 */
typedef struct {
    MfBundle *bundle;
    MfBundleImage *image;
} MfdBundleSource;

/**
 * mfd_bundle_source_free:
 */
static void
mfd_bundle_source_free(MfdBundleSource *source)
{
    g_object_unref(source->bundle);
    g_object_unref(source->image);
    g_free(source);
}

/**
 * mfd_write_bundle_image:
 *
 * Copy the image of an item out of its bundle.
 */
static gboolean
mfd_write_bundle_image(MfdInstallItem *item,
                       int fd,
                       MfCopyProgressFunc progress_func,
                       gpointer progress_data,
                       GError **error)
{
    MfdBundleSource *source = item->source;

    return mf_bundle_copy_image(source->bundle, source->image, fd, progress_func, progress_data, error);
}

/**
 * mfd_install_item_free:
 */
static void
mfd_install_item_free(MfdInstallItem *item)
{
    g_object_unref(item->manifest);
    g_free(item->name);
    g_free(item->origin);
    g_clear_pointer(&item->catalog, g_bytes_unref);
    g_clear_pointer(&item->icons, g_hash_table_unref);
    if (item->source_free != NULL)
        item->source_free(item->source);
    g_free(item);
}

/**
 * mfd_install_set_has_app_on:
 *
 * Returns: %TRUE if the set has an application that runs on the kit of @manifest
 */
static gboolean
mfd_install_set_has_app_on(MfdInstallSet *set, MfManifest *manifest)
{
    if (mf_manifest_get_kind(manifest) != MF_COMPONENT_KIND_KIT || mf_manifest_is_sdk(manifest))
        return FALSE;

    for (guint i = 0; i < set->items->len; i++) {
        MfManifest *app = ((MfdInstallItem *) g_ptr_array_index(set->items, i))->manifest;

        if (mf_manifest_get_kind(app) == MF_COMPONENT_KIND_APP && !mf_manifest_is_sdk(app) &&
            g_strcmp0(mf_manifest_get_base_identity(app), mf_manifest_get_identity(manifest)) == 0)
            return TRUE;
    }

    return FALSE;
}

/**
 * mfd_install_set_new_for_bundle:
 * @bundle: a bundle, read and verified
 *
 * Create a set from the images of a bundle. A kit that is in the same bundle
 * as an application which runs on it is installed as a dependency.
 *
 * Returns: (transfer full): the set
 */
MfdInstallSet *
mfd_install_set_new_for_bundle(MfBundle *bundle)
{
    GPtrArray *images = mf_bundle_get_images(bundle);
    MfdInstallSet *set = g_new0(MfdInstallSet, 1);

    set->items = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_install_item_free);
    for (guint i = 0; i < images->len; i++) {
        MfBundleImage *image = g_ptr_array_index(images, i);
        MfdInstallItem *item = g_new0(MfdInstallItem, 1);
        MfdBundleSource *source = g_new0(MfdBundleSource, 1);

        source->bundle = g_object_ref(bundle);
        source->image = g_object_ref(image);
        item->manifest = g_object_ref(mf_bundle_image_get_manifest(image));
        item->name = g_strdup(mf_bundle_image_get_manifest_name(image));
        item->origin = g_strdup(MF_BUNDLE_CATALOG_ORIGIN);
        item->catalog = g_bytes_ref(mf_bundle_image_get_catalog(image));
        item->icons = g_hash_table_ref(mf_bundle_image_get_icons(image));
        item->write_image = mfd_write_bundle_image;
        item->source = source;
        item->source_free = (GDestroyNotify) mfd_bundle_source_free;
        g_ptr_array_add(set->items, item);
    }

    for (guint i = 0; i < set->items->len; i++) {
        MfdInstallItem *item = g_ptr_array_index(set->items, i);

        item->is_dependency = mfd_install_set_has_app_on(set, item->manifest);
    }

    return set;
}

/**
 * mfd_install_set_free:
 */
void
mfd_install_set_free(MfdInstallSet *set)
{
    if (set == NULL)
        return;
    g_ptr_array_unref(set->items);
    g_free(set);
}

/**
 * mfd_install_set_find_identity:
 *
 * Returns: (transfer none) (nullable): the item that provides @identity
 */
MfdInstallItem *
mfd_install_set_find_identity(MfdInstallSet *set, const char *identity)
{
    for (guint i = 0; i < set->items->len; i++) {
        MfdInstallItem *item = g_ptr_array_index(set->items, i);

        if (g_strcmp0(mf_manifest_get_identity(item->manifest), identity) == 0)
            return item;
    }

    return NULL;
}

/**
 * mfd_install_set_find_exact_base:
 *
 * Returns: (transfer none) (nullable): the item with the component, version
 *   and revision that @manifest names as its base
 */
MfdInstallItem *
mfd_install_set_find_exact_base(MfdInstallSet *set, MfManifest *manifest)
{
    for (guint i = 0; i < set->items->len; i++) {
        MfdInstallItem *item = g_ptr_array_index(set->items, i);
        MfManifest *base = item->manifest;

        if (g_str_equal(mf_manifest_get_component_id(base), mf_manifest_get_base_id(manifest)) &&
            g_str_equal(mf_manifest_get_version(base), mf_manifest_get_base_version(manifest)) &&
            mf_manifest_get_revision(base) == mf_manifest_get_base_revision(manifest))
            return item;
    }

    return NULL;
}

/**
 * mfd_install_set_is_privileged:
 *
 * Returns: %TRUE if a component of the set is privileged
 */
gboolean
mfd_install_set_is_privileged(MfdInstallSet *set)
{
    for (guint i = 0; i < set->items->len; i++) {
        MfdInstallItem *item = g_ptr_array_index(set->items, i);
        MfIntegration *integration = mf_manifest_get_integration(item->manifest);

        if (integration != NULL && mf_integration_is_privileged(integration))
            return TRUE;
    }

    return FALSE;
}
