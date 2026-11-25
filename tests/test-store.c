/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <fcntl.h>
#include <inttypes.h>
#include <glib/gstdio.h>
#include <libglnx.h>
#include <manifold.h>

#include "mf-store-private.h"
#include "mft-sandbox.h"
#include "mft-utils.h"
#include "mfd-install-set.h"
#include "mfd-manager.h"
#include "mfd-modify-plan.h"
#include "mfd-store-writer.h"
#include "mfd-utils.h"

#define IMAGES   MF_STORAGE_DIR "/" MF_STORE_IMAGES_SUBDIR
#define APPS     MF_STORE_APPS_DIR
#define RUNTIMES MF_STORE_RUNTIMES_DIR

/* a kit that only an old version of the application uses */
#define OLD_KIT_ID TEST_CORE_ID ".0123456789abcdef"

/* the sizes of the images that the tests install */
#define CORE_IMAGE_SIZE 4096
#define KIT_IMAGE_SIZE  2048
#define APP_IMAGE_SIZE  1024

/* the store of the sandbox */
static MfStore *store = NULL;
static MfdStoreWriter *writer = NULL;
static MfdManager *manager = NULL;

/**
 * mft_reset_store:
 *
 * Remove everything from the store.
 */
static void
mft_reset_store(void)
{
    g_autoptr(GError) error = NULL;

    mft_clear_store();
    /* the directories were deleted, so the store has to drop its cached components */
    mf_store_invalidate(store);
    g_assert_true(mfd_store_writer_ensure_root(writer, &error));
    g_assert_no_error(error);
}

/**
 * mft_reload_store:
 *
 * Make the store read the installed components again, after a test
 * changed files of the store by hand.
 */
static void
mft_reload_store(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_true(mf_store_load(store, &error));
    g_assert_no_error(error);
}

static void
mft_add_image(const char *hash, size_t size)
{
    g_autofree char *path = g_strdup_printf(IMAGES "/%s.raw", hash);
    g_autofree char *data = g_strnfill(size, 'x');

    g_assert_true(g_file_set_contents(path, data, (ssize_t) size, NULL));
}

/**
 * mft_image_hash:
 *
 * Returns: (transfer full): the root hash of the image that mft_install() creates
 *   for a version and revision of a component
 */
static char *
mft_image_hash(const char *component_id, const char *version, uint64_t revision)
{
    g_autofree char *seed = g_strdup_printf("%s %s-%" PRIu64, component_id, version, revision);

    return mft_fake_hash(seed);
}

static GBytes *
mft_catalog_of(const char *component_id)
{
    g_autofree char *component = mft_catalog_component_xml(component_id, NULL, NULL);
    char *xml = mft_catalog_xml(component);

    return g_bytes_new_take(xml, strlen(xml));
}

/**
 * mft_install:
 * @manifest: the manifest of the component, without the root hash of its image
 * @base_version_dir: the version directory of the base that the `base` link points to,
 *   or %NULL for "current"
 * @mode: the install mode to record
 *
 * Create the image of a component, and install the component with the store
 * writer. The base of the component is always a runtime.
 */
static void
mft_install(const MftTestManifest *manifest, const char *base_version_dir, MfInstallMode mode)
{
    g_autofree char *hash = mft_image_hash(manifest->component_id,
                                           manifest->version,
                                           manifest->revision != 0 ? manifest->revision : 1);
    MftTestManifest with_image = *manifest;
    g_autoptr(GBytes) catalog = mft_catalog_of(manifest->component_id);
    g_autoptr(GHashTable) icons = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) parsed = NULL;
    g_autoptr(MfBaseLink) base = NULL;
    size_t image_size = APP_IMAGE_SIZE;

    if (g_str_equal(manifest->kind, "core"))
        image_size = CORE_IMAGE_SIZE;
    else if (g_str_equal(manifest->kind, "kit"))
        image_size = KIT_IMAGE_SIZE;
    mft_add_image(hash, image_size);

    with_image.verity_root_hash = hash;
    parsed = mft_manifest_new(&with_image);
    if (manifest->base_id != NULL)
        base = mf_base_link_new(MF_STORE_RUNTIMES_SUBDIR, manifest->base_id, base_version_dir);
    g_assert_true(mfd_store_writer_install_component(writer,
                                                     parsed,
                                                     base,
                                                     NULL,
                                                     catalog,
                                                     icons,
                                                     NULL,
                                                     MF_BUNDLE_CATALOG_ORIGIN,
                                                     mode,
                                                     &error));
    g_assert_no_error(error);
}

/**
 * mft_install_core:
 *
 * Install a version of the core with the install mode manual.
 */
static void
mft_install_core(const char *version)
{
    mft_install(&(MftTestManifest){.component_id = TEST_CORE_ID, .kind = "core", .version = version},
                NULL,
                MF_INSTALL_MODE_MANUAL);
}

/**
 * mft_install_core_sdk:
 *
 * Install the SDK of the core, which needs version 13.4 of the core.
 */
static void
mft_install_core_sdk(void)
{
    mft_install(&(MftTestManifest){.component_id = TEST_SDK_ID,
                                   .kind = "core",
                                   .version = "13.4",
                                   .sdk = TRUE,
                                   .base_id = TEST_CORE_ID,
                                   .base_kind = "core"},
                "13.4-1",
                MF_INSTALL_MODE_MANUAL);
}

/**
 * mft_install_kit:
 *
 * Install a version of a kit on the core.
 */
static void
mft_install_kit(const char *kit_id, const char *version, MfInstallMode mode)
{
    mft_install(&(MftTestManifest){.component_id = kit_id,
                                   .kind = "kit",
                                   .version = version,
                                   .base_id = TEST_CORE_ID,
                                   .base_kind = "core"},
                NULL,
                mode);
}

/**
 * mft_install_app:
 * @revision: the revision of the version, or 0 for the first revision
 *
 * Install a version of an application on a kit. An application is always
 * installed with the install mode auto.
 */
static void
mft_install_app(const char *app_id, const char *version, uint64_t revision, const char *kit_id)
{
    mft_install(&(MftTestManifest){.component_id = app_id,
                                   .kind = "app",
                                   .version = version,
                                   .revision = revision,
                                   .base_id = kit_id,
                                   .base_kind = "kit"},
                NULL,
                MF_INSTALL_MODE_AUTO);
}

/**
 * mft_install_usual:
 *
 * Empty the store, and install the usual setup: a manually installed core,
 * and an app with a kit that was installed automatically as its dependency.
 */
static void
mft_install_usual(void)
{
    mft_reset_store();
    mft_install_core("13.4");
    mft_install_kit(TEST_KIT_ID, "20260112T0238", MF_INSTALL_MODE_AUTO);
    mft_install_app(TEST_APP_ID, "0.1.0", 0, TEST_KIT_ID);
}

static void
mft_assert_link(const char *path, const char *expected_target)
{
    g_autofree char *target = g_file_read_link(path, NULL);

    g_assert_cmpstr(target, ==, expected_target);
}

/**
 * mft_assert_file:
 *
 * Check the content of a text file.
 */
static void
mft_assert_file(const char *path, const char *expected_content)
{
    g_autofree char *content = mft_read_file(path);

    g_assert_cmpstr(content, ==, expected_content);
}

/**
 * mft_list_components:
 *
 * List all installed components.
 */
static GPtrArray *
mft_list_components(void)
{
    g_autoptr(GError) error = NULL;
    GPtrArray *components = mf_store_list_components(store, NULL, MF_COMPONENT_KIND_UNKNOWN, &error);

    g_assert_no_error(error);
    return components;
}

static MfComponent *
mft_find_component(GPtrArray *components, const char *component_id, const char *version)
{
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (g_strcmp0(mf_component_get_id(component), component_id) == 0 &&
            g_strcmp0(mf_component_get_version(component), version) == 0)
            return component;
    }
    return NULL;
}

/**
 * mft_count_versions:
 *
 * Count the versions of a component that the store reader lists.
 */
static guint
mft_count_versions(const char *component_id)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray)
        components = mf_store_list_components(store, component_id, MF_COMPONENT_KIND_UNKNOWN, &error);

    g_assert_no_error(error);
    return components->len;
}

/**
 * mft_plan:
 * @component_id: (nullable): the component to remove, or %NULL to only clean up
 *
 * Plan a removal, and assert that the removal is possible.
 *
 * Returns: (transfer full): the removal plan
 */
static MfdRemovalPlan *
mft_plan(const char *component_id, const char *version)
{
    g_autoptr(GError) error = NULL;
    MfdRemovalPlan *rmplan = mfd_removal_plan_new_for_removal(store, component_id, version, &error);

    g_assert_no_error(error);
    return rmplan;
}

/**
 * mft_plan_find:
 * @dir: the version directory of a component
 *
 * Returns: the position of a component in a removal plan, or -1 if the plan does not remove it
 */
static int
mft_plan_find(MfdRemovalPlan *rmplan, const char *dir)
{
    for (guint i = 0; i < rmplan->items->len; i++) {
        MfdRemovalItem *item = g_ptr_array_index(rmplan->items, i);

        if (g_str_equal(mf_component_get_dir(item->component), dir))
            return (int) i;
    }
    return -1;
}

/**
 * mft_assert_in_use:
 *
 * Check that a version of a component can not be removed, as @used_by needs it.
 */
static void
mft_assert_in_use(const char *component_id, const char *version, const char *used_by)
{
    g_autoptr(GError) error = NULL;

    g_assert_null(mfd_removal_plan_new_for_removal(store, component_id, version, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_COMPONENT_IN_USE);
    g_assert_cmpstr(mf_error_get_field(error, MF_ERROR_FIELD_COMPONENT_ID), ==, component_id);
    g_assert_cmpstr(mf_error_get_field(error, MF_ERROR_FIELD_USED_BY), ==, used_by);
}

/**
 * test_install:
 *
 * Check the files that the store writer creates for installed components.
 */
static void
test_install(void)
{
    g_autofree char *core_hash = mft_image_hash(TEST_CORE_ID, "13.4", 1);
    g_autofree char *image_link = g_strdup_printf("../../../images/by-verity/%s.raw", core_hash);
    MftTestManifest core_manifest = {.component_id = TEST_CORE_ID,
                                     .kind = "core",
                                     .version = "13.4",
                                     .verity_root_hash = core_hash};
    g_autofree char *core_json = mft_manifest_json(&core_manifest);
    g_autoptr(GPtrArray) components = NULL;

    mft_install_usual();
    mft_install_core_sdk();

    /* the version directory of the core */
    mft_assert_link(RUNTIMES "/" TEST_CORE_ID "/current", "13.4-1");
    mft_assert_link(RUNTIMES "/" TEST_CORE_ID "/13.4-1/runtime.raw", image_link);
    mft_assert_file(RUNTIMES "/" TEST_CORE_ID "/13.4-1/manifest.json", core_json);
    mft_assert_file(RUNTIMES "/" TEST_CORE_ID "/13.4-1/origin", "manifold-local\n");
    g_assert_false(g_file_test(RUNTIMES "/" TEST_CORE_ID "/13.4-1/base", G_FILE_TEST_EXISTS));

    /* a base link points to the current version of the base, or to an exact version for an SDK */
    mft_assert_link(APPS "/" TEST_APP_ID "/0.1.0-1/base", "../../../runtimes/" TEST_KIT_ID "/current");
    mft_assert_link(RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1/base",
                    "../../../runtimes/" TEST_CORE_ID "/current");
    mft_assert_link(RUNTIMES "/" TEST_SDK_ID "/13.4-1/base", "../../../runtimes/" TEST_CORE_ID "/13.4-1");

    /* every version records its install mode */
    mft_assert_file(RUNTIMES "/" TEST_CORE_ID "/13.4-1/mode", "manual\n");
    mft_assert_file(RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1/mode", "auto\n");
    mft_assert_file(APPS "/" TEST_APP_ID "/0.1.0-1/mode", "auto\n");

    /* the store reader finds all of them */
    components = mft_list_components();
    g_assert_cmpuint(components->len, ==, 4);
    g_assert_cmpstr(mf_component_get_full_version(mft_find_component(components, TEST_CORE_ID, "13.4")),
                    ==,
                    "13.4-1");
    g_assert_true(mf_component_is_sdk(mft_find_component(components, TEST_SDK_ID, "13.4")));
    g_assert_true(mf_store_has_image(store, core_hash));
}

/**
 * test_install_again:
 *
 * A manual installation switches an automatically installed version to
 * manual, and an automatic installation never switches it back.
 */
static void
test_install_again(void)
{
    g_autoptr(MfComponent) kit = NULL;

    mft_install_usual();

    mft_install_kit(TEST_KIT_ID, "20260112T0238", MF_INSTALL_MODE_MANUAL);
    mft_assert_file(RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1/mode", "manual\n");
    mft_install_kit(TEST_KIT_ID, "20260112T0238", MF_INSTALL_MODE_AUTO);
    mft_assert_file(RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1/mode", "manual\n");
    kit = mf_store_get_component(store, TEST_KIT_ID, "20260112T0238", NULL);
    g_assert_cmpint(mf_component_get_install_mode(kit), ==, MF_INSTALL_MODE_MANUAL);

    /* installing an app version that is already installed does not add another version */
    mft_install_app(TEST_APP_ID, "0.1.0", 0, TEST_KIT_ID);
    g_assert_cmpuint(mft_count_versions(TEST_APP_ID), ==, 1);
}

/**
 * test_find_identity:
 *
 * An installed component can be found by its identity.
 */
static void
test_find_identity(void)
{
    g_autofree char *core_identity = mft_fake_hash(TEST_CORE_ID);
    g_autofree char *app_identity = mft_fake_hash(TEST_APP_ID);
    g_autofree char *other_identity = mft_fake_hash("nothing");
    g_autofree char *core_hash = mft_image_hash(TEST_CORE_ID, "13.4", 1);
    g_autofree char *app_hash = mft_image_hash(TEST_APP_ID, "0.1.0", 1);
    g_autofree char *app_image_path = g_strdup_printf(IMAGES "/%s.raw", app_hash);
    g_autoptr(MfComponent) core = NULL;
    g_autoptr(MfComponent) app = NULL;
    g_autoptr(MfComponent) current = NULL;
    g_autoptr(MfComponent) previous = NULL;
    g_autoptr(GError) error = NULL;

    mft_install_usual();

    core = mf_store_find_identity(store, core_identity, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_verity_hash(core), ==, core_hash);
    g_assert_cmpint(mf_component_get_kind(core), ==, MF_COMPONENT_KIND_CORE);
    g_assert_null(mf_store_find_identity(store, other_identity, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_clear_error(&error);

    /* apps can be found by their identity too, but only while their image exists */
    app = mf_store_find_identity(store, app_identity, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_verity_hash(app), ==, app_hash);
    g_assert_cmpint(mf_component_get_kind(app), ==, MF_COMPONENT_KIND_APP);
    g_assert_cmpint(g_unlink(app_image_path), ==, 0);
    g_assert_null(mf_store_find_identity(store, app_identity, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_clear_error(&error);

    /* if several versions have the identity, the current version is found */
    mft_install_core("13.6");
    current = mf_store_find_identity(store, core_identity, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_version(current), ==, "13.6");
    g_assert_true(mf_component_is_current(current));

    /* if the current version has a different identity, the newest version that still has it is found */
    mft_replace_in_file(RUNTIMES "/" TEST_CORE_ID "/13.6-1/manifest.json", core_identity, other_identity);
    mf_store_invalidate(store);
    previous = mf_store_find_identity(store, core_identity, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_version(previous), ==, "13.4");
}

/**
 * test_current_is_newest:
 *
 * The `current` link of a component always points to its newest version.
 */
static void
test_current_is_newest(void)
{
    g_autoptr(GPtrArray) components = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    mft_install_usual();

    mft_install_app(TEST_APP_ID, "0.2.0", 0, TEST_KIT_ID);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.2.0-1");

    /* versions are compared by version rules, not as strings: 0.10.0 is newer than 0.2.0,
     * and installing the older 0.3.0 afterwards does not change current */
    mft_install_app(TEST_APP_ID, "0.10.0", 0, TEST_KIT_ID);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.10.0-1");
    mft_install_app(TEST_APP_ID, "0.3.0", 0, TEST_KIT_ID);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.10.0-1");

    /* a rebuild of the same version has a higher revision, and the highest revision becomes current */
    mft_install_app(TEST_APP_ID, "0.10.0", 22910000, TEST_KIT_ID);
    mft_install_app(TEST_APP_ID, "0.10.0", 22910119, TEST_KIT_ID);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.10.0-22910119");
    g_assert_cmpuint(mft_count_versions(TEST_APP_ID), ==, 6);

    components = mft_list_components();
    g_assert_cmpuint(mf_component_get_revision(mft_find_component(components, TEST_APP_ID, "0.2.0")), ==, 1);
    g_assert_false(mf_component_is_current(mft_find_component(components, TEST_APP_ID, "0.2.0")));

    /* removing the current version, with all of its revisions, makes the next newest version current */
    result = mfd_manager_remove(manager, TEST_APP_ID, "0.10.0", NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.3.0-1");
}

/**
 * mft_count_finished:
 *
 * Count the items that a removal reported as finished.
 */
static void
mft_count_finished(const MfProgress *progress, gpointer user_data)
{
    const MfItemProgress *item = mf_progress_get_item(progress);
    guint *n_finished = user_data;

    g_assert_nonnull(item);
    /* an item is reported when it starts and when it finishes, so we only count finished items */
    if (mf_item_progress_get_percent(item) >= 100)
        (*n_finished)++;
}

/**
 * test_remove:
 *
 * Removing an application also removes the kit that was installed for it,
 * and the images that no component uses anymore.
 */
static void
test_remove(void)
{
    g_autofree char *stray_hash = mft_fake_hash("stray");
    g_autofree char *core_hash = mft_image_hash(TEST_CORE_ID, "13.4", 1);
    g_autofree char *kit_hash = mft_image_hash(TEST_KIT_ID, "20260112T0238", 1);
    g_autofree char *kit_image = g_strdup_printf(IMAGES "/%s.raw", kit_hash);
    g_autoptr(MfdRemovalPlan) cleanup = NULL;
    g_autoptr(MfdRemovalPlan) removal = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;
    guint n_finished = 0;

    mft_install_usual();
    mft_add_image(stray_hash, 8192);

    /* the core can not be removed, as the kit needs it and no other version of the core is installed */
    mft_assert_in_use(TEST_CORE_ID, "13.4", TEST_KIT_ID);

    /* a component and a version that are not installed */
    g_assert_null(mfd_removal_plan_new_for_removal(store, "org.example.nothing", NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_cmpstr(mf_error_get_field(error, MF_ERROR_FIELD_COMPONENT_ID), ==, "org.example.nothing");
    g_assert_null(mf_error_get_field(error, MF_ERROR_FIELD_USED_BY));
    g_clear_error(&error);
    g_assert_null(mfd_removal_plan_new_for_removal(store, TEST_CORE_ID, "9.9", &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_clear_error(&error);

    /* a cleanup that finds no unneeded component only removes the images that no component uses */
    cleanup = mft_plan(NULL, NULL);
    g_assert_cmpuint(cleanup->items->len, ==, 0);
    g_assert_cmpuint(cleanup->images->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(cleanup->images, 0), ==, stray_hash);
    g_assert_cmpuint(cleanup->image_bytes, ==, 8192);

    /* the app is removed on request, and the kit because no component needs it anymore */
    removal = mft_plan(TEST_APP_ID, NULL);
    g_assert_cmpuint(removal->items->len, ==, 2);
    g_assert_cmpint(mft_plan_find(removal, APPS "/" TEST_APP_ID "/0.1.0-1"), ==, 0);
    g_assert_cmpint(mft_plan_find(removal, RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1"), ==, 1);
    g_assert_cmpint(((MfdRemovalItem *) g_ptr_array_index(removal->items, 0))->action, ==, MF_ACTION_REMOVE);
    g_assert_cmpint(((MfdRemovalItem *) g_ptr_array_index(removal->items, 1))->action, ==, MF_ACTION_CLEANUP);
    g_assert_cmpuint(removal->images->len, ==, 3);
    g_assert_cmpuint(removal->image_bytes, ==, APP_IMAGE_SIZE + KIT_IMAGE_SIZE + 8192);

    /* leftovers of interrupted installations: in the app's directory, which is removed completely,
     * and in the core's directory, which stays */
    g_assert_cmpint(g_mkdir(APPS "/" TEST_APP_ID "/.tmp-9.9.9-1", 0755), ==, 0);
    g_assert_cmpint(g_mkdir(RUNTIMES "/" TEST_CORE_ID "/.tmp-13.6-1", 0755), ==, 0);

    /* remove the app */
    result = mfd_manager_remove(manager, TEST_APP_ID, NULL, mft_count_finished, &n_finished, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(n_finished, ==, 2);
    g_assert_cmpuint(mf_result_get_removed(result)->len, ==, 2);
    g_assert_cmpuint(mf_result_get_removed_images(result)->len, ==, 3);
    g_assert_cmpuint(mf_result_get_bytes_freed(result), ==, APP_IMAGE_SIZE + KIT_IMAGE_SIZE + 8192);

    /* the app and kit directories are removed with their last version; the core stays, but its
     * leftover is removed */
    g_assert_false(g_file_test(APPS "/" TEST_APP_ID, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(RUNTIMES "/" TEST_KIT_ID, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(RUNTIMES "/" TEST_CORE_ID "/13.4-1", G_FILE_TEST_IS_DIR));
    g_assert_false(g_file_test(RUNTIMES "/" TEST_CORE_ID "/.tmp-13.6-1", G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(kit_image, G_FILE_TEST_EXISTS));
    g_assert_true(mf_store_has_image(store, core_hash));
}

/**
 * test_used_base_stays:
 *
 * A component is not removed while another component is built on it.
 */
static void
test_used_base_stays(void)
{
    g_autofree char *app_hash = mft_image_hash(TEST_APP_ID, "0.1.0", 1);
    g_autoptr(MfdRemovalPlan) removal = NULL;

    mft_install_usual();
    mft_install_app("com.example.other", "1.0", 0, TEST_KIT_ID);
    mft_install_core("13.6");
    mft_install_core_sdk();

    /* the other application still needs the kit */
    removal = mft_plan(TEST_APP_ID, NULL);
    g_assert_cmpuint(removal->items->len, ==, 1);
    g_assert_cmpuint(removal->images->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(removal->images, 0), ==, app_hash);

    /* the SDK needs core 13.4, even though 13.6 is installed too */
    mft_assert_in_use(TEST_CORE_ID, "13.4", TEST_SDK_ID);

    /* the kit follows the current version of the core, and removing 13.6 would downgrade
     * the core under the kit */
    mft_assert_in_use(TEST_CORE_ID, "13.6", TEST_KIT_ID);
}

/**
 * test_cleanup:
 *
 * A cleanup removes the automatically installed versions that no component
 * needs anymore. An application is always installed automatically, so its
 * older versions are removed too, unless they were set to manual.
 */
static void
test_cleanup(void)
{
    g_autofree char *kit_hash = mft_image_hash(TEST_KIT_ID, "20260112T0238", 1);
    g_autoptr(MfdRemovalPlan) cleanup = NULL;
    int old_app;
    int old_kit;

    mft_install_usual();
    /* a newer version of the kit, which the application follows */
    mft_install_kit(TEST_KIT_ID, "20261101T0000", MF_INSTALL_MODE_AUTO);
    /* an old version of the application, on a kit that nothing else uses */
    mft_install_kit(OLD_KIT_ID, "20251201T0000", MF_INSTALL_MODE_AUTO);
    mft_install_app(TEST_APP_ID, "0.0.1", 0, OLD_KIT_ID);
    /* two newer versions of the application, and 0.1.0 is set to manual */
    mft_install_app(TEST_APP_ID, "0.2.0", 0, TEST_KIT_ID);
    mft_install_app(TEST_APP_ID, "0.3.0", 0, TEST_KIT_ID);
    mft_write_file(APPS "/" TEST_APP_ID "/0.1.0-1/mode", "manual\n");
    mft_reload_store();

    /* the newest version 0.3.0 and the manual version 0.1.0 of the application stay */
    cleanup = mft_plan(NULL, NULL);
    g_assert_cmpuint(cleanup->items->len, ==, 4);
    g_assert_cmpint(mft_plan_find(cleanup, APPS "/" TEST_APP_ID "/0.2.0-1"), >=, 0);
    g_assert_cmpint(mft_plan_find(cleanup, RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1"), >=, 0);
    g_assert_true(g_ptr_array_find_with_equal_func(cleanup->images, kit_hash, g_str_equal, NULL));
    g_assert_cmpuint(cleanup->images->len, ==, 4);

    /* a chain of dependencies is removed in one run, and the application before its kit */
    old_app = mft_plan_find(cleanup, APPS "/" TEST_APP_ID "/0.0.1-1");
    old_kit = mft_plan_find(cleanup, RUNTIMES "/" OLD_KIT_ID "/20251201T0000-1");
    g_assert_cmpint(old_app, >=, 0);
    g_assert_cmpint(old_kit, >, old_app);
}

/**
 * test_update_gains_privileges:
 *
 * An update that adds privileged files to an application needs the approval
 * of an admin, unless an installed version of the application has privileged
 * files already.
 */
static void
test_update_gains_privileges(void)
{
    /* clang-format off */
    const char *rule =
        "{\"udev-rule\": [{"
        "  \"path\": \"/app/lib/udev/rules.d/60-" TEST_APP_ID ".rules\","
        "  \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "  \"blake3\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\""
        "}]}";
    /* clang-format on */
    MftTestManifest app = {.component_id = TEST_APP_ID,
                           .kind = "app",
                           .version = "0.2.0",
                           .base_id = TEST_KIT_ID,
                           .base_kind = "kit"};
    MftTestManifest app_with_rule = app;
    MftTestManifest other_with_rule = app;
    g_autoptr(MfManifest) plain = NULL;
    g_autoptr(MfManifest) with_rule = NULL;
    g_autoptr(MfManifest) other = NULL;
    g_autoptr(MfdInstallSet) set = mfd_install_set_new();

    app_with_rule.integration = rule;
    other_with_rule.component_id = "org.example.other";
    other_with_rule.integration = rule;
    plain = mft_manifest_new(&app);
    with_rule = mft_manifest_new(&app_with_rule);
    other = mft_manifest_new(&other_with_rule);

    mft_install_usual();

    /* an update without privileged files */
    mfd_install_set_add(set, plain, "app", "example-main");
    g_assert_false(mfd_manager_install_set_gains_privileges(manager, set));

    /* the installed version has no udev rule, so the update makes the application privileged */
    mfd_install_set_add(set, with_rule, "app", "example-main");
    g_assert_true(mfd_manager_install_set_gains_privileges(manager, set));
    g_clear_pointer(&set, mfd_install_set_free);

    /* once a version with the rule is installed, an update that has it needs no approval */
    app_with_rule.version = "0.1.5";
    mft_install(&app_with_rule, NULL, MF_INSTALL_MODE_AUTO);
    set = mfd_install_set_new();
    mfd_install_set_add(set, with_rule, "app", "example-main");
    g_assert_false(mfd_manager_install_set_gains_privileges(manager, set));

    /* a privileged component that is not installed at all needs approval */
    mfd_install_set_add(set, other, "other", "example-main");
    g_assert_true(mfd_manager_install_set_gains_privileges(manager, set));
}

/**
 * mft_assert_not_a_file_name:
 *
 * Check that a write was refused because of the name of its file.
 */
static void
mft_assert_not_a_file_name(GError **error)
{
    g_assert_error(*error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr((*error)->message, "is not the name of a file"));
    g_clear_error(error);
}

/**
 * test_writes_stay_below_their_directory:
 *
 * The file and link helpers of the daemon never write outside of their
 * target directory.
 */
static void
test_writes_stay_below_their_directory(void)
{
    g_autofree char *dir = mft_make_tmp_dir("store-writes");
    g_autofree char *inside = g_build_filename(dir, "inside", NULL);
    g_autofree char *outside = g_build_filename(dir, "outside", NULL);
    g_autofree char *outside_file = g_build_filename(outside, "file", NULL);
    g_autofree char *outside_sub = g_build_filename(outside, "sub", NULL);
    g_autofree char *escaped = g_build_filename(dir, "escaped", NULL);
    g_autofree char *tmp_dir = g_build_filename(inside, MF_TMP_PREFIX "..", NULL);
    g_autofree char *link = g_build_filename(inside, "link", NULL);
    g_autofree char *up = g_build_filename(inside, "up", NULL);
    g_autofree char *self = g_build_filename(inside, "self", NULL);
    g_autoptr(GError) error = NULL;
    g_autofd int inside_fd = -1;
    g_autofd int sub_fd = -1;

    mft_make_dirs(inside);
    mft_make_dirs(outside);

    /* a file name must not contain a path, so it can not lead out of the directory */
    g_assert_false(mfd_write_contents_safe(inside, "../escaped", "x", -1, &error));
    mft_assert_not_a_file_name(&error);
    /* the temporary link would be `.tmp-../escaped`, which can only be created if the directory
     * `.tmp-..` exists */
    mft_make_dirs(tmp_dir);
    g_assert_false(mfd_replace_link("target", inside, "../escaped", &error));
    mft_assert_not_a_file_name(&error);
    g_assert_false(g_file_test(escaped, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK));

    /* an existing link with the file name is replaced, and its target is not written to */
    mft_make_link(outside_file, link);
    g_assert_true(mfd_write_contents_safe(inside, "link", "x", -1, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(outside_file, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(link, G_FILE_TEST_IS_SYMLINK));

    /* a subdirectory path can not lead out of the directory either, by `..` or by a link */
    g_assert_true(glnx_opendirat(AT_FDCWD, inside, TRUE, &inside_fd, &error));
    g_assert_cmpint(mfd_make_dir_below(inside_fd, "../outside/sub", &error), ==, -1);
    g_assert_nonnull(error);
    g_clear_error(&error);
    mft_make_link("../outside", up);
    g_assert_cmpint(mfd_make_dir_below(inside_fd, "up/sub", &error), ==, -1);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(g_file_test(outside_sub, G_FILE_TEST_EXISTS));

    /* no link is followed at all, even if it points into the directory */
    mft_make_link(".", self);
    g_assert_cmpint(mfd_make_dir_below(inside_fd, "self/sub", &error), ==, -1);
    g_assert_nonnull(error);
    g_clear_error(&error);

    /* a path below the directory */
    sub_fd = mfd_make_dir_below(inside_fd, "48x48/apps", &error);
    g_assert_no_error(error);
    g_assert_cmpint(sub_fd, >=, 0);
    g_assert_true(mfd_write_file_at(sub_fd, "icon.png", "png", 3, 0644, &error));
    g_assert_no_error(error);

    mft_rm_rf(dir);
}

int
main(int argc, char **argv)
{
    mft_sandbox_enter();
    g_test_init(&argc, &argv, NULL);
    store = mf_store_new();
    writer = mfd_store_writer_new(store);
    manager = mfd_manager_new_for_store(store);

    g_test_add_func("/store/install", test_install);
    g_test_add_func("/store/install-again", test_install_again);
    g_test_add_func("/store/find-identity", test_find_identity);
    g_test_add_func("/store/current-is-newest", test_current_is_newest);
    g_test_add_func("/store/remove", test_remove);
    g_test_add_func("/store/used-base-stays", test_used_base_stays);
    g_test_add_func("/store/cleanup", test_cleanup);
    g_test_add_func("/store/update-gains-privileges", test_update_gains_privileges);
    g_test_add_func("/store/writes-stay-below-their-directory", test_writes_stay_below_their_directory);

    return g_test_run();
}
