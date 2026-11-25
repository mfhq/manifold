/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>
#include <manifold.h>

#include "mf-manifest-private.h"
#include "mft-utils.h"

#define HASH(digit) \
    digit digit digit digit digit digit digit digit digit digit digit digit digit digit digit digit

/**
 * mft_read_sample:
 *
 * Returns: (transfer full): the text of a file in the samples directory
 */
static char *
mft_read_sample(const char *name)
{
    return mft_read_file(g_test_get_filename(G_TEST_DIST, "samples", name, NULL));
}

/**
 * mft_read_manifest:
 *
 * Returns: (transfer full): the parsed manifest of a file in the samples directory
 */
static MfManifest *
mft_read_manifest(const char *name)
{
    const char *path = g_test_get_filename(G_TEST_DIST, "samples", name, NULL);
    g_autoptr(GError) error = NULL;
    MfManifest *manifest = mf_manifest_new_from_file(path, &error);

    g_assert_no_error(error);
    return manifest;
}

/**
 * mft_app_json_with:
 *
 * Returns: (transfer full): the manifest of the application, with @old replaced by @new in its text
 */
static char *
mft_app_json_with(const char *old, const char *new)
{
    g_autofree char *json = mft_read_sample("app.manifold.json");

    return mft_str_replace(json, old, new);
}

/**
 * mft_assert_refused:
 * @message_part: a part of the expected error message
 *
 * Check that a manifest is refused when it is parsed.
 */
static void
mft_assert_refused(const char *json, const char *message_part)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) manifest = mf_manifest_new_from_data(json, -1, &error);

    g_assert_null(manifest);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
    g_assert_nonnull(strstr(error->message, message_part));
    /* a manifest must not be able to control the terminal of the admin */
    g_assert_null(strchr(error->message, '\033'));
}

/**
 * mft_assert_app_refused:
 *
 * Check that the manifest of the application is refused once @old is
 * replaced by @new in its text.
 */
static void
mft_assert_app_refused(const char *old, const char *new, const char *message_part)
{
    g_autofree char *json = mft_app_json_with(old, new);

    mft_assert_refused(json, message_part);
}

/**
 * mft_app_manifest_with_permissions:
 * @permissions: the permissions list as JSON
 *
 * Returns: (transfer full): the manifest of the application, with other permissions
 */
static MfManifest *
mft_app_manifest_with_permissions(const char *permissions)
{
    g_autofree char *json = mft_app_json_with("[\"host-commands\"]", permissions);
    g_autoptr(GError) error = NULL;
    MfManifest *manifest = mf_manifest_new_from_data(json, -1, &error);

    g_assert_no_error(error);
    return manifest;
}

/**
 * mft_assert_permissions_refused:
 * @message_part: a part of the expected error message
 *
 * Check that mf_manifest_check_permissions() refuses a manifest.
 */
static void
mft_assert_permissions_refused(MfManifest *manifest, const char *message_part)
{
    g_autoptr(GError) error = NULL;

    g_assert_false(mf_manifest_check_permissions(manifest, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
    g_assert_nonnull(strstr(error->message, message_part));
    g_assert_null(strchr(error->message, '\033'));
}

/**
 * test_manifest_read:
 *
 * Read the manifest of an application.
 */
static void
test_manifest_read(void)
{
    g_autoptr(MfManifest) manifest = mft_read_manifest("app.manifold.json");
    g_autofree char *json = mft_read_sample("app.manifold.json");
    g_autoptr(GError) error = NULL;
    const MfBaseReference *base;

    g_assert_cmpmem(g_bytes_get_data(mf_manifest_get_data(manifest), NULL),
                    g_bytes_get_size(mf_manifest_get_data(manifest)),
                    json,
                    strlen(json));
    g_assert_cmpstr(mf_manifest_get_component_id(manifest), ==, "com.example.app");
    g_assert_cmpint(mf_manifest_get_kind(manifest), ==, MF_COMPONENT_KIND_APP);
    g_assert_cmpstr(mf_manifest_get_version(manifest), ==, "0.1.0");
    g_assert_cmpuint(mf_manifest_get_revision(manifest), ==, 22931700);
    g_assert_cmpstr(mf_manifest_get_architecture(manifest), ==, "x86_64");
    g_assert_false(mf_manifest_is_sdk(manifest));
    g_assert_cmpstr(mf_manifest_get_identity(manifest), ==, HASH("1111"));
    g_assert_cmpstr(mf_manifest_get_content_hash(manifest), ==, HASH("2222"));
    g_assert_cmpstr(mf_manifest_get_command(manifest), ==, "app");

    /* an application works with any version of its base that has the identity */
    base = mf_manifest_get_base(manifest);
    g_assert_cmpstr(base->id, ==, "net.debian.core13.7d225c1fe79b1c5e");
    g_assert_cmpint(base->kind, ==, MF_COMPONENT_KIND_KIT);
    g_assert_cmpstr(base->version, ==, "20260112T0238");
    g_assert_cmpuint(base->revision, ==, 22910051);
    g_assert_cmpstr(base->identity, ==, HASH("3333"));
    g_assert_cmpstr(base->content_hash, ==, HASH("4444"));
    g_assert_false(base->exact);
    g_assert_null(mf_manifest_get_base_sdk(manifest));

    /* image */
    g_assert_cmpstr(mf_manifest_get_image_file(manifest), ==, "com.example.app_0.1.0.raw");
    g_assert_cmpuint(mf_manifest_get_image_size(manifest), ==, 28672);
    g_assert_cmpstr(mf_manifest_get_image_checksum(manifest, MF_CHECKSUM_KIND_SHA256), ==, HASH("7777"));
    g_assert_cmpstr(mf_manifest_get_image_checksum(manifest, MF_CHECKSUM_KIND_BLAKE3), ==, HASH("8888"));
    g_assert_cmpstr(mf_manifest_get_verity_hash(manifest), ==, HASH("9999"));
    g_assert_cmpuint(mf_manifest_get_data_offset(manifest), ==, 20480);
    g_assert_cmpuint(mf_manifest_get_data_size(manifest), ==, 4096);
    g_assert_cmpuint(mf_manifest_get_hash_offset(manifest), ==, 24576);
    g_assert_cmpuint(mf_manifest_get_hash_size(manifest), ==, 4096);

    /* integration */
    g_assert_nonnull(mf_manifest_get_integration(manifest));
    g_assert_true(mf_manifest_check_integration(manifest, &error));
    g_assert_no_error(error);
}

/**
 * test_manifest_read_sdk:
 *
 * Read the manifest of an SDK that extends a platform. The SDK names the
 * platform as its base, and the SDK of the core as its base SDK.
 */
static void
test_manifest_read_sdk(void)
{
    g_autoptr(MfManifest) manifest = mft_read_manifest("platform-sdk.manifold.json");
    const MfBaseReference *base = mf_manifest_get_base(manifest);
    const MfBaseReference *base_sdk = mf_manifest_get_base_sdk(manifest);

    g_assert_cmpint(mf_manifest_get_kind(manifest), ==, MF_COMPONENT_KIND_PLATFORM);
    g_assert_true(mf_manifest_is_sdk(manifest));
    g_assert_null(mf_manifest_get_command(manifest));
    g_assert_null(mf_manifest_get_integration(manifest));

    /* an SDK needs the exact versions that it was built on */
    g_assert_cmpstr(base->id, ==, "net.debian.core13.kde6");
    g_assert_cmpint(base->kind, ==, MF_COMPONENT_KIND_PLATFORM);
    g_assert_true(base->exact);
    g_assert_cmpstr(base_sdk->id, ==, "net.debian.core13.sdk");
    g_assert_cmpint(base_sdk->kind, ==, MF_COMPONENT_KIND_CORE);
    g_assert_cmpstr(base_sdk->version, ==, "13.8");
    g_assert_cmpuint(base_sdk->revision, ==, 22910000);
    g_assert_cmpstr(base_sdk->identity, ==, HASH("5555"));
    g_assert_cmpstr(base_sdk->content_hash, ==, HASH("6666"));
    g_assert_true(base_sdk->exact);

    /* the sizes of the partitions are optional */
    g_assert_cmpuint(mf_manifest_get_data_size(manifest), ==, 0);
    g_assert_cmpuint(mf_manifest_get_hash_size(manifest), ==, 0);
}

/**
 * test_manifest_refused:
 *
 * A manifest that is not well-formed is refused when it is parsed.
 */
static void
test_manifest_refused(void)
{
    g_autofree char *kit_json = mft_manifest_json(
        &(MftTestManifest){.component_id = TEST_KIT_ID, .kind = "kit", .version = "1"});

    mft_assert_refused("not json", "");
    mft_assert_app_refused("\"image\": {", "\"no-image\": {", "image is missing");
    mft_assert_app_refused("\"hash-offset\": 24576", "\"hash-offset\": 1", "placed before the data");

    /* only a core has no base */
    mft_assert_refused(kit_json, "a kit must name the layer it is built on");

    /* the value of a field never reaches the terminal unescaped */
    mft_assert_app_refused("\"kind\": \"app\"", "\"kind\": \"\\u001b[2J\"", "kind");
    mft_assert_app_refused("\"kind\": \"kit\"", "\"kind\": \"\\u001b[2J\"", "base: kind");
    mft_assert_app_refused("\"command\": \"app\"", "\"command\": \"\\u001b[2J\"", "command");
    mft_assert_app_refused("\"architecture\": \"x86_64\"",
                           "\"architecture\": \"\\u001b[2J\"",
                           "architecture");
}

/**
 * test_permissions:
 *
 * A manifest can request permissions for its application. The permissions
 * list is checked when the image is built or installed, and not when the
 * manifest is parsed.
 */
static void
test_permissions(void)
{
    g_autoptr(MfManifest) manifest = mft_read_manifest("app.manifold.json");
    g_autoptr(MfManifest) none = mft_app_manifest_with_permissions("[]");
    g_autoptr(MfManifest) unknown = mft_app_manifest_with_permissions("[\"host-tmp\", \"\\u001b[2J\"]");
    g_autoptr(MfManifest) implied = mft_app_manifest_with_permissions("[\"udev-rules\"]");
    g_autoptr(MfManifest) core = mft_manifest_new(&(MftTestManifest){.component_id = TEST_CORE_ID,
                                                                     .kind = "core",
                                                                     .version = "13.8",
                                                                     .permissions = "[\"host-commands\"]"});
    g_autoptr(GError) error = NULL;

    g_assert_true(mf_manifest_has_permission(manifest, MF_PERMISSION_HOST_COMMANDS));
    g_assert_false(mf_manifest_has_permission(manifest, MF_PERMISSION_HOST_TMP));
    g_assert_false(mf_manifest_is_privileged(manifest));
    g_assert_true(mf_manifest_check_permissions(manifest, &error));
    g_assert_no_error(error);

    g_assert_false(mf_manifest_has_permission(none, MF_PERMISSION_HOST_COMMANDS));
    g_assert_true(mf_manifest_check_permissions(none, &error));
    g_assert_no_error(error);

    /* an unknown permission is only refused by the check, the known ones are still read */
    g_assert_true(mf_manifest_has_permission(unknown, MF_PERMISSION_HOST_TMP));
    mft_assert_permissions_refused(unknown, "is not known");

    /* a manifest can not request a permission that its integration section implies */
    g_assert_false(mf_manifest_has_permission(implied, MF_PERMISSION_UDEV_RULES));
    g_assert_false(mf_manifest_is_privileged(implied));
    mft_assert_permissions_refused(implied, "udev-rules can not be requested");

    /* only applications are launched */
    mft_assert_permissions_refused(core, "only applications can use this");

    /* not a list */
    mft_assert_app_refused("[\"host-commands\"]", "\"host-commands\"", "permissions is not a list");
}

/**
 * test_permission_strings:
 *
 * Test the names and properties of the permissions.
 */
static void
test_permission_strings(void)
{
    for (MfPermission permission = MF_PERMISSION_UNKNOWN + 1; permission < MF_PERMISSION_LAST; permission++)
        g_assert_cmpint(mf_permission_from_string(mf_permission_to_string(permission)), ==, permission);
    g_assert_cmpstr(mf_permission_to_string(MF_PERMISSION_LAST), ==, "unknown");
    g_assert_cmpint(mf_permission_from_string("unknown"), ==, MF_PERMISSION_UNKNOWN);
    g_assert_cmpint(mf_permission_from_string(NULL), ==, MF_PERMISSION_UNKNOWN);

    g_assert_false(mf_permission_is_implied(MF_PERMISSION_HOST_COMMANDS));
    g_assert_false(mf_permission_is_implied(MF_PERMISSION_HOST_TMP));
    g_assert_true(mf_permission_is_implied(MF_PERMISSION_SYSTEM_SERVICES));
    g_assert_true(mf_permission_is_implied(MF_PERMISSION_UDEV_RULES));

    g_assert_true(mf_permission_is_privileged(MF_PERMISSION_SYSTEM_SERVICES));
    g_assert_true(mf_permission_is_privileged(MF_PERMISSION_UDEV_RULES));
    g_assert_false(mf_permission_is_privileged(MF_PERMISSION_HOST_COMMANDS));
    g_assert_false(mf_permission_is_privileged(MF_PERMISSION_UNKNOWN));
    g_assert_false(mf_permission_is_privileged(MF_PERMISSION_LAST));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/manifest/read", test_manifest_read);
    g_test_add_func("/manifest/read-sdk", test_manifest_read_sdk);
    g_test_add_func("/manifest/refused", test_manifest_refused);
    g_test_add_func("/manifest/permissions", test_permissions);
    g_test_add_func("/manifest/permission-strings", test_permission_strings);

    return g_test_run();
}
