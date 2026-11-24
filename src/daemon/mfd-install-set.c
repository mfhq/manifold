/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-install-set.h"

#include "mf-bundle-private.h"
#include "mf-manifest-private.h"
#include "mf-store-private.h"
#include "mf-utils-private.h"

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
 * mfd_install_set_has_app_on_kit:
 *
 * Returns: %TRUE if the set has an application that runs on the kit of @manifest
 */
static gboolean
mfd_install_set_has_app_on_kit(MfdInstallSet *set, MfManifest *manifest)
{
    if (mf_manifest_get_kind(manifest) != MF_COMPONENT_KIND_KIT || mf_manifest_is_sdk(manifest))
        return FALSE;

    for (guint i = 0; i < set->items->len; i++) {
        MfManifest *app = ((MfdInstallItem *) g_ptr_array_index(set->items, i))->manifest;
        const MfBaseReference *base = mf_manifest_get_base(app);

        if (mf_manifest_get_kind(app) == MF_COMPONENT_KIND_APP && !mf_manifest_is_sdk(app) &&
            g_str_equal(base->identity, mf_manifest_get_identity(manifest)))
            return TRUE;
    }

    return FALSE;
}

/**
 * mfd_install_set_new:
 *
 * Returns: (transfer full): an empty set
 */
MfdInstallSet *
mfd_install_set_new(void)
{
    MfdInstallSet *set = g_new0(MfdInstallSet, 1);

    set->items = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_install_item_free);
    set->warnings = g_ptr_array_new_with_free_func(g_free);
    return set;
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
    MfdInstallSet *set = mfd_install_set_new();

    for (guint i = 0; i < images->len; i++) {
        MfBundleImage *image = g_ptr_array_index(images, i);
        MfdInstallItem *item = mfd_install_set_add(set,
                                                   mf_bundle_image_get_manifest(image),
                                                   mf_bundle_image_get_manifest_name(image),
                                                   MF_BUNDLE_CATALOG_ORIGIN);
        MfdBundleSource *source = g_new0(MfdBundleSource, 1);

        source->bundle = g_object_ref(bundle);
        source->image = g_object_ref(image);
        item->catalog = g_bytes_ref(mf_bundle_image_get_catalog(image));
        item->icons = g_hash_table_ref(mf_bundle_image_get_icons(image));
        item->write_image = mfd_write_bundle_image;
        item->source = source;
        item->source_free = (GDestroyNotify) mfd_bundle_source_free;
    }

    for (guint i = 0; i < set->items->len; i++) {
        MfdInstallItem *item = g_ptr_array_index(set->items, i);

        item->is_dependency = mfd_install_set_has_app_on_kit(set, item->manifest);
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
    g_ptr_array_unref(set->warnings);
    g_free(set);
}

/**
 * mfd_install_set_add:
 * @manifest: the manifest of the component
 * @name: the name of the item in log and error messages
 * @origin: the origin of the component
 *
 * Add an item to the set. The caller has to set the function that writes its image.
 *
 * Returns: (transfer none): the new item, owned by the set
 */
MfdInstallItem *
mfd_install_set_add(MfdInstallSet *set, MfManifest *manifest, const char *name, const char *origin)
{
    MfdInstallItem *item = g_new0(MfdInstallItem, 1);

    item->manifest = g_object_ref(manifest);
    item->name = g_strdup(name);
    item->origin = g_strdup(origin);
    g_ptr_array_add(set->items, item);
    return item;
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
 * mfd_install_set_find_version:
 *
 * Returns: (transfer none) (nullable): the item for one version and revision of a component
 */
MfdInstallItem *
mfd_install_set_find_version(MfdInstallSet *set,
                             const char *component_id,
                             const char *version,
                             uint64_t revision)
{
    for (guint i = 0; i < set->items->len; i++) {
        MfdInstallItem *item = g_ptr_array_index(set->items, i);

        if (g_str_equal(mf_manifest_get_component_id(item->manifest), component_id) &&
            g_str_equal(mf_manifest_get_version(item->manifest), version) &&
            mf_manifest_get_revision(item->manifest) == revision)
            return item;
    }

    return NULL;
}

/**
 * mfd_check_if_pending_item_wins:
 * @pending: the manifest of an item of the set that provides an identity
 * @installed: (nullable): the installed component that provides the same identity, or %NULL
 *
 * Check whether we use an item of the set to provide an identity, instead of
 * the installed component that provides the same identity. The item wins,
 * unless the installed component has the same ID as the item, and is the
 * same or a newer version.
 *
 * Returns: %TRUE if the item of the set wins
 */
gboolean
mfd_check_if_pending_item_wins(MfManifest *pending, MfComponent *installed)
{
    return installed == NULL ||
           g_strcmp0(mf_component_get_id(installed), mf_manifest_get_component_id(pending)) != 0 ||
           mf_version_compare_with_rev(mf_component_get_version(installed),
                                       mf_component_get_revision(installed),
                                       mf_manifest_get_version(pending),
                                       mf_manifest_get_revision(pending)) < 0;
}

/**
 * mfd_base_provider_clear:
 */
void
mfd_base_provider_clear(MfdBaseProvider *provider)
{
    provider->item = NULL;
    g_clear_object(&provider->installed);
}

/**
 * mfd_install_set_find_base:
 * @ref: a reference to a base or to a base SDK
 * @ret_provider: (out caller-allocates): the component that is used for @ref
 *
 * Find the component that is used for a base reference, in the set or in
 * the store.
 *
 * An exact reference only accepts the version and revision that it names,
 * and that version must provide the identity of the reference. Any other
 * reference accepts every component that provides its identity. If an item
 * of the set and an installed component both provide the identity,
 * mfd_check_if_pending_item_wins() decides which one is used.
 *
 * Returns: %TRUE if a component for @ref was found
 */
gboolean
mfd_install_set_find_base(MfdInstallSet *set,
                          MfStore *store,
                          const MfBaseReference *ref,
                          MfdBaseProvider *ret_provider)
{
    MfdInstallItem *item;
    g_autoptr(MfComponent) installed = NULL;

    if (ref->exact) {
        const char *identity = NULL;

        item = mfd_install_set_find_version(set, ref->id, ref->version, ref->revision);
        if (item != NULL) {
            identity = mf_manifest_get_identity(item->manifest);
        } else {
            installed = mf_store_find_version(store, ref->id, ref->version, ref->revision, NULL);
            if (installed != NULL)
                identity = mf_component_get_identity(installed);
        }
        if (g_strcmp0(identity, ref->identity) != 0)
            return FALSE;
    } else {
        item = mfd_install_set_find_identity(set, ref->identity);
        installed = mf_store_find_identity(store, ref->identity, NULL);
        if (item != NULL && mfd_check_if_pending_item_wins(item->manifest, installed))
            g_clear_object(&installed);
        else
            item = NULL;
        if (item == NULL && installed == NULL)
            return FALSE;
    }

    ret_provider->item = item;
    ret_provider->installed = g_steal_pointer(&installed);
    return TRUE;
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

        if (mf_manifest_is_privileged(item->manifest))
            return TRUE;
    }

    return FALSE;
}
