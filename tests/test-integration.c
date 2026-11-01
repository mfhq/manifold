/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <manifold.h>

#include "mft-utils.h"

#define SHA256_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define BLAKE3_A "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define HASHES   "\"sha256\": \"" SHA256_A "\", \"blake3\": \"" BLAKE3_A "\""

/**
 * test_manifest_new:
 * @integration: (nullable): the integration section as JSON
 *
 * Returns: (transfer full) (nullable): the parsed manifest of an application
 */
static MfManifest *
test_manifest_new(const char *integration, GError **error)
{
    MftTestManifest m = {
        .component_id = "com.example.app",
        .kind = "app",
        .version = "1.0",
        .base_id = "net.example.core",
        .integration = integration,
    };
    g_autofree char *json = mft_manifest_json(&m);

    return mf_manifest_new_from_data(json, -1, error);
}

static void
test_kind_strings(void)
{
    for (MfIntegrationKind kind = MF_INTEGRATION_KIND_UNKNOWN + 1; kind < MF_INTEGRATION_KIND_LAST; kind++)
        g_assert_cmpint(mf_integration_kind_from_string(mf_integration_kind_to_string(kind)), ==, kind);
    g_assert_cmpstr(mf_integration_kind_to_string(MF_INTEGRATION_KIND_LAST), ==, "unknown");
    g_assert_cmpint(mf_integration_kind_from_string("unknown"), ==, MF_INTEGRATION_KIND_UNKNOWN);
    g_assert_cmpint(mf_integration_kind_from_string("alias"), ==, MF_INTEGRATION_KIND_UNKNOWN);
    g_assert_cmpint(mf_integration_kind_from_string(NULL), ==, MF_INTEGRATION_KIND_UNKNOWN);
}

static void
test_section(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) manifest = test_manifest_new(
        "{\"desktop-entry\": [{\"path\": \"/app/share/applications/com.example.app.desktop\", " HASHES
        "}],"
        " \"systemd-unit-user\": [{\"path\": \"/app/lib/systemd/user/com.example.app.service\", " HASHES
        "}],"
        " \"alias\": [{\"name\": \"app\", \"command\": \"/app/bin/app\"}],"
        " \"from-the-future\": 42}",
        &error);
    MfIntegration *integration;
    GPtrArray *files;
    GPtrArray *aliases;
    GPtrArray *unknown;
    MfIntegrationFile *file;
    MfIntegrationAlias *alias;

    g_assert_no_error(error);
    integration = mf_manifest_get_integration(manifest);
    g_assert_nonnull(integration);

    files = mf_integration_get_files(integration);
    g_assert_cmpuint(files->len, ==, 2);
    file = g_ptr_array_index(files, 0);
    g_assert_cmpint(mf_integration_file_get_kind(file), ==, MF_INTEGRATION_KIND_DESKTOP_ENTRY);
    g_assert_cmpstr(mf_integration_file_get_path(file),
                    ==,
                    "/app/share/applications/com.example.app.desktop");
    g_assert_cmpstr(mf_integration_file_get_checksum(file, MF_CHECKSUM_KIND_SHA256), ==, SHA256_A);
    g_assert_cmpstr(mf_integration_file_get_checksum(file, MF_CHECKSUM_KIND_BLAKE3), ==, BLAKE3_A);
    file = g_ptr_array_index(files, 1);
    g_assert_cmpint(mf_integration_file_get_kind(file), ==, MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER);

    aliases = mf_integration_get_aliases(integration);
    g_assert_cmpuint(aliases->len, ==, 1);
    alias = g_ptr_array_index(aliases, 0);
    g_assert_cmpstr(mf_integration_alias_get_name(alias), ==, "app");
    g_assert_cmpstr(mf_integration_alias_get_command(alias), ==, "/app/bin/app");

    /* the value of a kind we do not know is not looked at */
    unknown = mf_integration_get_unknown_kinds(integration);
    g_assert_cmpuint(unknown->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(unknown, 0), ==, "from-the-future");
}

static void
test_no_section(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) without = test_manifest_new(NULL, &error);
    g_autoptr(MfManifest) null = test_manifest_new("null", &error);
    g_autoptr(MfManifest) empty = test_manifest_new("{}", &error);

    g_assert_no_error(error);
    g_assert_null(mf_manifest_get_integration(without));
    g_assert_null(mf_manifest_get_integration(null));
    g_assert_cmpuint(mf_integration_get_files(mf_manifest_get_integration(empty))->len, ==, 0);
}

static void
test_malformed_section(void)
{
    static const char *const sections[] = {
        "[]",
        "\"desktop-entry\"",
        "{\"desktop-entry\": {}}",
        "{\"desktop-entry\": \"/app/x\"}",
        "{\"desktop-entry\": [\"/app/x\"]}",
        "{\"desktop-entry\": [{" HASHES "}]}",
        "{\"desktop-entry\": [{\"path\": 1, " HASHES "}]}",
        "{\"desktop-entry\": [{\"path\": \"/app/x\"}]}",
        "{\"desktop-entry\": [{\"path\": \"/app/x\", \"sha256\": \"" SHA256_A "\"}]}",
        "{\"desktop-entry\": [{\"path\": \"/app/x\", \"sha256\": \"short\", \"blake3\": \"" BLAKE3_A "\"}]}",
        "{\"alias\": {}}",
        "{\"alias\": [\"app\"]}",
        "{\"alias\": [{\"name\": \"app\"}]}",
        "{\"alias\": [{\"command\": \"/app/bin/app\"}]}",
    };

    for (gsize i = 0; i < G_N_ELEMENTS(sections); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(MfManifest) manifest = test_manifest_new(sections[i], &error);

        g_test_message("%s", sections[i]);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(manifest);
    }
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/integration/kind-strings", test_kind_strings);
    g_test_add_func("/integration/section", test_section);
    g_test_add_func("/integration/no-section", test_no_section);
    g_test_add_func("/integration/malformed-section", test_malformed_section);

    return g_test_run();
}
