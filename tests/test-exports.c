/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>
#include <manifold.h>

#include "mf-integration-private.h"
#include "mft-utils.h"

/* Tests for the `integration` section of a manifest, which lists the files
 * and aliases that an application exports to the host. */

#define SHA256_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define BLAKE3_A "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define HASHES   "'sha256': '" SHA256_A "', 'blake3': '" BLAKE3_A "'"

/* the command lines that run the application, or one of its programs, from the host */
#define RUN_APP      MF_MFRUN_PATH " com.example.app"
#define RUN(program) MF_MFRUN_PATH " --command=" program " com.example.app"

/**
 * mft_json:
 *
 * The tests write JSON with single quotes, so the text needs no escapes.
 *
 * Returns: (transfer full): @text with double quotes in place of the single quotes
 */
static char *
mft_json(const char *text)
{
    return g_strdelimit(g_strdup(text), "'", '"');
}

/**
 * mft_file_section:
 * @kind: the key of the kind of the file
 * @path: the path of the file in the image
 *
 * Returns: (transfer full): an integration section with one file, as JSON
 */
static char *
mft_file_section(const char *kind, const char *path)
{
    g_autofree char *hashes = mft_json(HASHES);

    return g_strdup_printf("{\"%s\": [{\"path\": \"%s\", %s}]}", kind, path, hashes);
}

/**
 * mft_app_manifest:
 * @integration: (nullable): the integration section as JSON
 *
 * Returns: the manifest of an application
 */
static MftTestManifest
mft_app_manifest(const char *integration)
{
    return (MftTestManifest){
        .component_id = "com.example.app",
        .kind = "app",
        .version = "1.0",
        .base_id = "net.example.core",
        .base_kind = "core",
        .integration = integration,
    };
}

/**
 * mft_app_manifest_new:
 * @section: (nullable): the integration section as JSON, written with single quotes
 *
 * Returns: (transfer full): the parsed manifest of an application
 */
static MfManifest *
mft_app_manifest_new(const char *section)
{
    g_autofree char *integration = section != NULL ? mft_json(section) : NULL;
    MftTestManifest m = mft_app_manifest(integration);

    return mft_manifest_new(&m);
}

/**
 * mft_passes_check:
 * @m: a manifest that can be parsed
 *
 * Returns: whether mf_manifest_check_integration() accepts the manifest
 */
static gboolean
mft_passes_check(const MftTestManifest *m)
{
    g_autoptr(MfManifest) manifest = mft_manifest_new(m);
    g_autoptr(GError) error = NULL;

    if (mf_manifest_check_integration(manifest, &error))
        return TRUE;
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
    /* a manifest must not be able to control the terminal of the admin */
    g_assert_null(strchr(error->message, '\033'));

    return FALSE;
}

/**
 * mft_section_passes_check:
 * @section: (nullable): the integration section as JSON, written with single quotes
 *
 * Returns: whether mf_manifest_check_integration() accepts an application with the section
 */
static gboolean
mft_section_passes_check(const char *section)
{
    g_autofree char *integration = section != NULL ? mft_json(section) : NULL;
    MftTestManifest m = mft_app_manifest(integration);

    return mft_passes_check(&m);
}

/**
 * mft_render:
 * @kind: the key of the kind of the file
 * @path: the path of the file in the image
 * @content: the content of the file
 *
 * Returns: (transfer full) (nullable): the file to install
 */
static GBytes *
mft_render(const char *kind, const char *path, const char *content, GError **error)
{
    g_autofree char *section = mft_file_section(kind, path);
    MftTestManifest m = mft_app_manifest(section);
    g_autoptr(MfManifest) manifest = mft_manifest_new(&m);
    g_autoptr(GBytes) bytes = g_bytes_new_static(content, strlen(content));
    g_autoptr(GError) local_error = NULL;
    MfIntegrationFile *file;

    g_assert_true(mf_manifest_check_integration(manifest, &local_error));
    g_assert_no_error(local_error);
    file = g_ptr_array_index(mf_integration_get_files(mf_manifest_get_integration(manifest)), 0);

    return mf_integration_file_render(file, manifest, bytes, error);
}

/**
 * mft_assert_renders:
 * @expected: the content of the file that is installed on the host
 *
 * Check that a file of an image is accepted, and what is installed for it.
 */
static void
mft_assert_renders(const char *kind, const char *path, const char *content, const char *expected)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) rendered = mft_render(kind, path, content, &error);
    g_autofree char *text = NULL;

    g_test_message("%s", content);
    g_assert_no_error(error);
    text = g_strndup(g_bytes_get_data(rendered, NULL), g_bytes_get_size(rendered));
    g_assert_cmpstr(text, ==, expected);
}

/**
 * mft_assert_render_refused:
 *
 * Check that a file of an image is refused because of its content.
 */
static void
mft_assert_render_refused(const char *kind, const char *path, const char *content)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) rendered = mft_render(kind, path, content, &error);

    g_test_message("%s", content);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
    g_assert_null(rendered);
}

/**
 * mft_assert_alias_renders:
 * @command: the program that the alias runs
 * @expected: (nullable): the script that is installed, or %NULL if the alias is refused
 *
 * Check the script that is installed for an alias.
 */
static void
mft_assert_alias_renders(const char *command, const char *expected)
{
    g_autofree char *section = g_strdup_printf("{\"alias\": [{\"name\": \"tool\", \"command\": \"%s\"}]}",
                                               command);
    MftTestManifest m = mft_app_manifest(section);
    g_autoptr(MfManifest) manifest = mft_manifest_new(&m);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) script = NULL;
    g_autofree char *text = NULL;

    script = mf_integration_alias_render(
        g_ptr_array_index(mf_integration_get_aliases(mf_manifest_get_integration(manifest)), 0),
        manifest,
        &error);
    if (expected == NULL) {
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(script);
        return;
    }
    g_assert_no_error(error);
    text = g_strndup(g_bytes_get_data(script, NULL), g_bytes_get_size(script));
    g_assert_cmpstr(text, ==, expected);
}

/**
 * test_kinds:
 *
 * Test the names and properties of the kinds of integration files.
 */
static void
test_kinds(void)
{
    for (MfIntegrationKind kind = MF_INTEGRATION_KIND_UNKNOWN + 1; kind < MF_INTEGRATION_KIND_LAST; kind++)
        g_assert_cmpint(mf_integration_kind_from_string(mf_integration_kind_to_string(kind)), ==, kind);
    g_assert_cmpstr(mf_integration_kind_to_string(MF_INTEGRATION_KIND_LAST), ==, "unknown");
    g_assert_cmpint(mf_integration_kind_from_string("unknown"), ==, MF_INTEGRATION_KIND_UNKNOWN);
    g_assert_cmpint(mf_integration_kind_from_string("alias"), ==, MF_INTEGRATION_KIND_UNKNOWN);
    g_assert_cmpint(mf_integration_kind_from_string(NULL), ==, MF_INTEGRATION_KIND_UNKNOWN);

    g_assert_cmpstr(mf_integration_kind_get_dir(MF_INTEGRATION_KIND_UDEV_RULE), ==, "lib/udev/rules.d");
    g_assert_null(mf_integration_kind_get_dir(MF_INTEGRATION_KIND_UNKNOWN));

    g_assert_true(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM));
    g_assert_true(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_UDEV_RULE));
    g_assert_false(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER));
    g_assert_false(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_DESKTOP_ENTRY));
    g_assert_false(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_UNKNOWN));
    g_assert_false(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_LAST));
}

/**
 * test_section:
 *
 * Read the integration section of a manifest.
 */
static void
test_section(void)
{
    /* clang-format off */
    g_autoptr(MfManifest) manifest = mft_app_manifest_new(
        "{"
        "  'desktop-entry': [{'path': '/app/share/applications/com.example.app.desktop', " HASHES "}],"
        "  'systemd-unit-user': [{'path': '/app/lib/systemd/user/com.example.app.service', " HASHES "}],"
        "  'alias': [{'name': 'app', 'command': '/app/bin/app'}],"
        "  'from-the-future': 42"
        "}");
    /* clang-format on */
    g_autoptr(MfManifest) without = mft_app_manifest_new(NULL);
    g_autoptr(MfManifest) null = mft_app_manifest_new("null");
    g_autoptr(MfManifest) empty = mft_app_manifest_new("{}");
    MfIntegration *integration = mf_manifest_get_integration(manifest);
    GPtrArray *files;
    GPtrArray *aliases;
    GPtrArray *unknown;
    MfIntegrationFile *file;
    MfIntegrationAlias *alias;

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

    /* the value of an unknown kind is not parsed */
    unknown = mf_integration_get_unknown_kinds(integration);
    g_assert_cmpuint(unknown->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(unknown, 0), ==, "from-the-future");

    /* no section, and an empty one */
    g_assert_null(mf_manifest_get_integration(without));
    g_assert_null(mf_manifest_get_integration(null));
    g_assert_cmpuint(mf_integration_get_files(mf_manifest_get_integration(empty))->len, ==, 0);
}

/**
 * test_malformed_section:
 *
 * A manifest with a malformed integration section is refused when it is parsed.
 */
static void
test_malformed_section(void)
{
    static const char *const sections[] = {
        "[]",
        "'desktop-entry'",
        "{'desktop-entry': {}}",
        "{'desktop-entry': '/app/x'}",
        "{'desktop-entry': ['/app/x']}",
        "{'desktop-entry': [{" HASHES "}]}",
        "{'desktop-entry': [{'path': 1, " HASHES "}]}",
        "{'desktop-entry': [{'path': '/app/x'}]}",
        "{'desktop-entry': [{'path': '/app/x', 'sha256': '" SHA256_A "'}]}",
        "{'desktop-entry': [{'path': '/app/x', 'sha256': 'short', 'blake3': '" BLAKE3_A "'}]}",
        "{'alias': {}}",
        "{'alias': ['app']}",
        "{'alias': [{'name': 'app'}]}",
        "{'alias': [{'command': '/app/bin/app'}]}",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(sections); i++) {
        g_autofree char *section = mft_json(sections[i]);
        MftTestManifest m = mft_app_manifest(section);
        g_autofree char *json = mft_manifest_json(&m);
        g_autoptr(GError) error = NULL;
        g_autoptr(MfManifest) manifest = mf_manifest_new_from_data(json, -1, &error);

        g_test_message("%s", sections[i]);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(manifest);
    }
}

/**
 * test_export_paths:
 *
 * The integration section is not checked when a manifest is read, so an
 * entry whose path leads out of the directory of its kind must have no
 * export path.
 */
static void
test_export_paths(void)
{
    /* clang-format off */
    g_autoptr(MfManifest) manifest = mft_app_manifest_new(
        "{"
        "  'desktop-entry': ["
        "    {'path': '/app/share/applications/com.example.app.desktop', " HASHES "},"
        "    {'path': '/app/share/applications/../../../etc/com.example.app.desktop', " HASHES "},"
        "    {'path': '/app/share/applications/./com.example.app.desktop', " HASHES "},"
        "    {'path': '/app/share/applications', " HASHES "}"
        "  ],"
        "  'alias': ["
        "    {'name': 'app', 'command': '/app/bin/app'},"
        "    {'name': '../app', 'command': '/app/bin/app'}"
        "  ]"
        "}");
    /* clang-format on */
    GPtrArray *files = mf_integration_get_files(mf_manifest_get_integration(manifest));
    GPtrArray *aliases = mf_integration_get_aliases(mf_manifest_get_integration(manifest));

    g_assert_cmpuint(files->len, ==, 4);
    g_assert_cmpstr(mf_integration_file_get_export_path(g_ptr_array_index(files, 0)),
                    ==,
                    "share/applications/com.example.app.desktop");
    for (guint i = 1; i < files->len; i++)
        g_assert_null(mf_integration_file_get_export_path(g_ptr_array_index(files, i)));

    g_assert_cmpstr(mf_integration_alias_get_export_path(g_ptr_array_index(aliases, 0)), ==, "bin/app");
    g_assert_null(mf_integration_alias_get_export_path(g_ptr_array_index(aliases, 1)));
    g_assert_false(mf_manifest_check_integration(manifest, NULL));
}

/**
 * test_privileged:
 *
 * A file that gives an image root access to the host makes its component
 * privileged, and implies the permission for it.
 */
static void
test_privileged(void)
{
    g_autoptr(MfManifest) rule = mft_app_manifest_new(
        "{'udev-rule': [{'path': '/app/lib/udev/rules.d/60-com.example.app.rules', " HASHES "}]}");
    g_autoptr(MfManifest) unit = mft_app_manifest_new(
        "{'systemd-unit-user': [{'path': '/app/lib/systemd/user/com.example.app.service', " HASHES "}]}");
    g_autoptr(GError) error = NULL;

    g_assert_true(mf_manifest_is_privileged(rule));
    g_assert_true(mf_manifest_has_permission(rule, MF_PERMISSION_UDEV_RULES));
    g_assert_false(mf_manifest_has_permission(rule, MF_PERMISSION_SYSTEM_SERVICES));
    g_assert_true(mf_manifest_check_permissions(rule, &error));
    g_assert_no_error(error);

    /* a user unit does not make a component privileged, as it only runs as a user */
    g_assert_false(mf_manifest_is_privileged(unit));
    g_assert_false(mf_manifest_has_permission(unit, MF_PERMISSION_SYSTEM_SERVICES));
}

/**
 * test_diff_privileged:
 *
 * Find the privileged files that an update adds and removes.
 */
static void
test_diff_privileged(void)
{
    /* clang-format off */
    g_autoptr(MfManifest) previous = mft_app_manifest_new(
        "{"
        "  'udev-rule': [{'path': '/app/lib/udev/rules.d/60-com.example.app.rules', " HASHES "}],"
        "  'systemd-unit-system': [{'path': '/app/lib/systemd/system/com.example.app.service', " HASHES "}],"
        "  'systemd-unit-user': [{'path': '/app/lib/systemd/user/com.example.app.service', " HASHES "}]"
        "}");
    g_autoptr(MfManifest) current = mft_app_manifest_new(
        "{"
        "  'udev-rule': [{'path': '/app/lib/udev/rules.d/60-com.example.app.rules', " HASHES "}],"
        "  'systemd-unit-system': [{'path': '/app/lib/systemd/system/com.example.app.timer', " HASHES "}]"
        "}");
    /* clang-format on */
    g_autoptr(GPtrArray) added = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) removed = g_ptr_array_new_with_free_func(g_free);

    /* the udev rule is in both versions and a user unit is not privileged, so neither is reported */
    mf_integration_diff_privileged(mf_manifest_get_integration(previous),
                                   mf_manifest_get_integration(current),
                                   added,
                                   removed);
    g_assert_cmpuint(added->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(added, 0), ==, "com.example.app.timer");
    g_assert_cmpuint(removed->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(removed, 0), ==, "com.example.app.service");

    /* a version without an integration section installs nothing */
    g_ptr_array_set_size(added, 0);
    g_ptr_array_set_size(removed, 0);
    mf_integration_diff_privileged(NULL, mf_manifest_get_integration(current), added, removed);
    g_assert_cmpuint(added->len, ==, 2);
    g_assert_cmpuint(removed->len, ==, 0);

    /* the same version twice */
    g_ptr_array_set_size(added, 0);
    mf_integration_diff_privileged(mf_manifest_get_integration(current),
                                   mf_manifest_get_integration(current),
                                   added,
                                   removed);
    g_assert_cmpuint(added->len, ==, 0);
    g_assert_cmpuint(removed->len, ==, 0);
}

/**
 * test_check_section:
 *
 * Test the rules for an integration section as a whole.
 */
static void
test_check_section(void)
{
    g_assert_true(mft_section_passes_check(NULL));
    g_assert_true(mft_section_passes_check("{}"));

    /* an unknown kind must not be dropped silently */
    g_assert_false(mft_section_passes_check("{'from-the-future': []}"));
    g_assert_false(mft_section_passes_check("{'\\u001b[2J': []}"));

    /* an alias and a file can only be listed once */
    g_assert_false(
        mft_section_passes_check("{'alias': [{'name': 'app', 'command': '/app/bin/a'},"
                                 "           {'name': 'app', 'command': '/app/bin/b'}]}"));
    g_assert_false(mft_section_passes_check(
        "{'mime-package': [{'path': '/app/share/mime/packages/com.example.app.xml', " HASHES
        "},"
        "                  {'path': '/app/share/mime/packages/com.example.app.xml', " HASHES "}]}"));
}

/**
 * test_check_component:
 *
 * Only an application with a full reverse-DNS name can have an integration section.
 */
static void
test_check_component(void)
{
    static const char *const refused_ids[] = {"ssh", "com.example", "com..example", "com.example."};
    /* every kind is tested with a valid base kind, so the kind is the only reason for the refusal */
    static const struct {
        const char *kind;
        const char *base_kind;
    } refused_kinds[] = {
        {"kit",      "core"},
        {"platform", "core"},
        {"plugin",   "app" }
    };
    MftTestManifest m;

    /* a short ID like `ssh` could claim a file of the host, like `ssh.service` */
    for (size_t i = 0; i < G_N_ELEMENTS(refused_ids); i++) {
        m = mft_app_manifest("{}");
        m.component_id = refused_ids[i];
        g_test_message("%s", refused_ids[i]);
        g_assert_false(mft_passes_check(&m));
    }

    /* only applications are launched */
    for (size_t i = 0; i < G_N_ELEMENTS(refused_kinds); i++) {
        m = mft_app_manifest("{}");
        m.kind = refused_kinds[i].kind;
        m.base_kind = refused_kinds[i].base_kind;
        g_test_message("%s", refused_kinds[i].kind);
        g_assert_false(mft_passes_check(&m));
    }

    /* the SDK of an application is not launched either */
    m = mft_app_manifest("{}");
    m.sdk = TRUE;
    g_assert_false(mft_passes_check(&m));

    /* an SDK without an integration section is fine */
    m.integration = NULL;
    g_assert_true(mft_passes_check(&m));
}

/**
 * test_check_file_paths:
 *
 * A file has to be in the directory of its kind, and its name has to start
 * with the ID of the component.
 */
static void
test_check_file_paths(void)
{
    static const struct {
        const char *kind;
        const char *path;
        gboolean valid;
    } files[] = {
        /* accepted paths */
        {"desktop-entry",       "/app/share/applications/com.example.app.desktop",                     TRUE },
        {"desktop-entry",       "/app/share/applications/com.example.app.open.desktop",                TRUE },
        {"desktop-entry",       "/app/share/applications/com.example.app-open.desktop",                TRUE },
        {"icon",                "/app/share/icons/hicolor/scalable/apps/com.example.app.svg",          TRUE },
        {"icon",                "/app/share/icons/hicolor/48x48/apps/com.example.app.png",             TRUE },
        {"icon",                "/app/share/icons/hicolor/48x48/apps/com.example.app.jxl",             TRUE },
        {"icon",                "/app/share/icons/hicolor/16x16@2x/apps/com.example.app.png",          TRUE },
        {"icon",                "/app/share/icons/hicolor/64x64@2/apps/com.example.app.png",           TRUE },
        {"icon",                "/app/share/icons/hicolor/symbolic/apps/com.example.app-symbolic.svg", TRUE },
        {"icon",                "/app/share/icons/hicolor/48x48/mimetypes/com.example.app.doc.png",    TRUE },
        {"mime-package",        "/app/share/mime/packages/com.example.app.xml",                        TRUE },
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.service",                     TRUE },
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app-report.service",              TRUE },
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app@.service",                    TRUE },
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app-worker@.service",             TRUE },
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.socket",                      TRUE },
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.timer",                       TRUE },
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.path",                        TRUE },
        {"systemd-unit-user",   "/app/lib/systemd/user/com.example.app.target",                        TRUE },
        {"udev-rule",           "/app/lib/udev/rules.d/60-com.example.app.rules",                      TRUE },
        {"udev-rule",           "/app/lib/udev/rules.d/99-com.example.app-extra.rules",                TRUE },

        /* paths outside of the directory of their kind */
        {"desktop-entry",       "/usr/share/applications/com.example.app.desktop",                     FALSE},
        {"desktop-entry",       "share/applications/com.example.app.desktop",                          FALSE},
        {"desktop-entry",       "/app/share/applications/kde/com.example.app.desktop",                 FALSE},
        {"desktop-entry",       "/app/share/applications//com.example.app.desktop",                    FALSE},
        {"desktop-entry",       "/app/share/applications/../applications/com.example.app.desktop",     FALSE},
        {"desktop-entry",       "/app/share/applications/../../../etc/com.example.app.desktop",        FALSE},
        {"systemd-unit-user",   "/app/lib/systemd/system/com.example.app.service",                     FALSE},
        {"systemd-unit-system", "/app/etc/systemd/system/com.example.app.service",                     FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.service.d/x.conf",            FALSE},
        {"icon",                "/app/share/icons/Adwaita/48x48/apps/com.example.app.png",             FALSE},
        {"icon",                "/app/share/icons/hicolor/apps/com.example.app.png",                   FALSE},
        {"icon",                "/app/share/icons/hicolor/big/apps/com.example.app.png",               FALSE},
        {"icon",                "/app/share/icons/hicolor/48x48/actions/com.example.app.png",          FALSE},
        {"icon",                "/app/share/icons/hicolor/48x48/apps/x/com.example.app.png",           FALSE},
        {"icon",                "/app/share/icons/hicolor/48x48\\n/apps/com.example.app.png",          FALSE},

        /* file names that are not named after the component */
        {"desktop-entry",       "/app/share/applications/org.gnome.Nautilus.desktop",                  FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.desktop",                         FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app2.desktop",                    FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.application.desktop",             FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app..desktop",                    FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app-.desktop",                    FALSE},
        {"desktop-entry",       "/app/share/applications/.desktop",                                    FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/ssh.service",                                 FALSE},
        {"mime-package",        "/app/share/mime/packages/freedesktop.org.xml",                        FALSE},
        {"udev-rule",           "/app/lib/udev/rules.d/60-mftestapp.rules",                            FALSE},
        {"udev-rule",           "/app/lib/udev/rules.d/60-other.rules",                                FALSE},

        /* file names without the suffix that the host looks for */
        {"desktop-entry",       "/app/share/applications/com.example.app",                             FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app.desktop.bak",                 FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app@.desktop",                    FALSE},
        {"icon",                "/app/share/icons/hicolor/48x48/apps/com.example.app.gif",             FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.mount",                       FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.slice",                       FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app@one.service",                 FALSE},
        {"udev-rule",           "/app/lib/udev/rules.d/com.example.app.rules",                         FALSE},
        {"udev-rule",           "/app/lib/udev/rules.d/6-com.example.app.rules",                       FALSE},
        {"udev-rule",           "/app/lib/udev/rules.d/600-com.example.app.rules",                     FALSE},

        /* file names with unsafe characters */
        {"desktop-entry",       "/app/share/applications/com.example.app .desktop",                    FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app.a'b.desktop",                 FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app.a\\nb.desktop",               FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app.a\\u001bb.desktop",           FALSE},
        {"desktop-entry",       "/tmp/\\u001b[2Jx",                                                    FALSE},
    };

    for (size_t i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree char *section = mft_file_section(files[i].kind, files[i].path);
        MftTestManifest m = mft_app_manifest(section);

        g_test_message("%s %s", files[i].kind, files[i].path);
        g_assert_cmpint(mft_passes_check(&m), ==, files[i].valid);
    }
}

/**
 * test_check_aliases:
 *
 * An alias has a plain name, and runs a program of the image.
 */
static void
test_check_aliases(void)
{
    static const struct {
        const char *name;
        const char *command;
        gboolean valid;
    } aliases[] = {
        {"app",        "/app/bin/app",          TRUE },
        {"app-cli.1",  "/app/bin/app",          TRUE },
        {"g++",        "/app/libexec/app/g++",  TRUE },

        /* an alias name that is not a safe file name */
        {"a/b",        "/app/bin/app",          FALSE},

        /* reserved names of tools that a host may lack */
        {"sudo",       "/app/bin/app",          FALSE},
        {"pkexec",     "/app/bin/app",          FALSE},
        {"run0",       "/app/bin/app",          FALSE},
        {"manifold",   "/app/bin/app",          FALSE},
        {"mfrun",      "/app/bin/app",          FALSE},

        /* commands that are not a program of the image, or that are not safe to put in a script */
        {"app",        "",                      FALSE},
        {"app",        "app",                   FALSE},
        {"app",        "bin/app",               FALSE},
        {"app",        "/usr/bin/app",          FALSE},
        {"app",        "/app",                  FALSE},
        {"app",        "/app/",                 FALSE},
        {"app",        "/app/bin/",             FALSE},
        {"app",        "/app//bin/app",         FALSE},
        {"app",        "/app/../bin/sh",        FALSE},
        {"app",        "/app/bin/../../bin/sh", FALSE},
        {"app",        "/app/bin/.hidden",      FALSE},
        {"app",        "/app/bin/a b",          FALSE},
        {"app",        "/app/bin/a'b",          FALSE},
        {"app",        "/app/bin/a\\nb",        FALSE},
        {"app",        "/app/bin/$(id)",        FALSE},

        /* names and commands that would control a terminal */
        {"\\u001b[2J", "/app/bin/app",          FALSE},
        {"app",        "\\u001b[2J",            FALSE},
    };

    for (size_t i = 0; i < G_N_ELEMENTS(aliases); i++) {
        g_autofree char *section = g_strdup_printf("{\"alias\": [{\"name\": \"%s\", \"command\": \"%s\"}]}",
                                                   aliases[i].name,
                                                   aliases[i].command);
        MftTestManifest m = mft_app_manifest(section);

        g_test_message("%s %s", aliases[i].name, aliases[i].command);
        g_assert_cmpint(mft_passes_check(&m), ==, aliases[i].valid);
    }
}

/**
 * test_render_aliases:
 *
 * An alias is installed as a script that runs its program in the image.
 */
static void
test_render_aliases(void)
{
    /* mfrun runs the program of the application by default */
    mft_assert_alias_renders("/app/bin/app", "#!/bin/sh\nexec " RUN_APP " \"$@\"\n");
    mft_assert_alias_renders("/app/libexec/tool", "#!/bin/sh\nexec " RUN("/app/libexec/tool") " \"$@\"\n");

    /* the script is installed into the PATH, so its command is checked again when it is rendered */
    mft_assert_alias_renders("/usr/bin/sh", NULL);
    mft_assert_alias_renders("/app/bin/$(id)", NULL);
}

#define ENTRY_DIR   "/app/share/applications/"
#define MAIN_ENTRY  ENTRY_DIR "com.example.app.desktop"
#define OTHER_ENTRY ENTRY_DIR "com.example.app.open.desktop"
#define HEADER      "[Desktop Entry]\nType=Application\nName=App\n"

/**
 * test_desktop_entries:
 *
 * The commands of a desktop entry are replaced with commands that run the
 * program in the image.
 */
static void
test_desktop_entries(void)
{
    /* clang-format off */
    static const struct {
        const char *path;
        const char *content;
        const char *expected;
    } entries[] = {
        /* the main entry runs the command of the application, given by its name or by its path */
        {MAIN_ENTRY,
         HEADER "Exec=app\n",
         HEADER "Exec=" RUN_APP "\n"},
        {MAIN_ENTRY,
         HEADER "Exec=/app/bin/app %U\n",
         HEADER "Exec=" RUN_APP " %U\n"},
        {MAIN_ENTRY,
         HEADER "Exec=app --flag \"a b\" %f\n",
         HEADER "Exec=" RUN_APP " --flag \"a b\" %f\n"},

        /* other entries can run the command of the application, or any other program of the image */
        {OTHER_ENTRY,
         HEADER "Exec=app --open %f\n",
         HEADER "Exec=" RUN_APP " --open %f\n"},
        {OTHER_ENTRY,
         HEADER "Exec=/app/libexec/tool\n",
         HEADER "Exec=" RUN("/app/libexec/tool") "\n"},
        {OTHER_ENTRY,
         HEADER "Exec=tool %u\n",
         HEADER "Exec=" RUN("tool") " %u\n"},

        /* the actions of the main entry can run any program of the image */
        {MAIN_ENTRY,
         HEADER "Exec=app\n"
                "Actions=Tool;\n"
                "\n"
                "[Desktop Action Tool]\n"
                "Name=Tool\n"
                "Exec=tool\n",
         HEADER "Exec=" RUN_APP "\n"
                "Actions=Tool;\n"
                "\n"
                "[Desktop Action Tool]\n"
                "Name=Tool\n"
                "Exec=" RUN("tool") "\n"},

        /* all other lines stay as they are, and the rendered file always ends in a newline */
        {MAIN_ENTRY,
         "# a comment\n"
         "\n"
         "[Desktop Entry]\n"
         "Type=Application\n"
         "Name=App\n"
         "Name[de_DE.UTF-8@euro]=Äpp\n"
         "X-KDE-Something=1\n"
         "Exec=app\n"
         "Icon=com.example.app\n"
         "MimeType=application/x-app;x-scheme-handler/app;\n"
         "Terminal=false",
         "# a comment\n"
         "\n"
         "[Desktop Entry]\n"
         "Type=Application\n"
         "Name=App\n"
         "Name[de_DE.UTF-8@euro]=Äpp\n"
         "X-KDE-Something=1\n"
         "Exec=" RUN_APP "\n"
         "Icon=com.example.app\n"
         "MimeType=application/x-app;x-scheme-handler/app;\n"
         "Terminal=false\n"},

        /* the program of TryExec is in the image, and not on the host */
        {MAIN_ENTRY,
         HEADER "TryExec=app\nExec=app\n",
         HEADER "Exec=" RUN_APP "\n"},
        {MAIN_ENTRY,
         HEADER "DBusActivatable=false\nExec=app\n",
         HEADER "DBusActivatable=false\nExec=" RUN_APP "\n"},
    };
    /* clang-format on */

    for (size_t i = 0; i < G_N_ELEMENTS(entries); i++)
        mft_assert_renders("desktop-entry", entries[i].path, entries[i].content, entries[i].expected);
}

/**
 * test_desktop_entries_refused:
 *
 * A desktop entry is refused if we can not be sure what the host will run for it.
 */
static void
test_desktop_entries_refused(void)
{
    static const struct {
        const char *path;
        const char *content;
    } entries[] = {
        /* lines that other parsers see differently */
        {MAIN_ENTRY,  HEADER "Comment=x\rExec=/bin/evil\nExec=app\n"                 },
        {MAIN_ENTRY,  HEADER "Comment=\xff\xfe\nExec=app\n"                          },
        {MAIN_ENTRY,  "\xef\xbb\xbf" HEADER "Exec=app\n"                             },
        {MAIN_ENTRY,  HEADER "Comment=x\\\nExec=app\n"                               },
        {MAIN_ENTRY,  HEADER "Exec=app\nExec=/bin/evil\n"                            },
        {MAIN_ENTRY,  HEADER "Exec=app\n[Desktop Entry]\nExec=/bin/evil\n"           },
        {MAIN_ENTRY,  HEADER "Exec=app\n Exec=/bin/evil\n"                           },
        {MAIN_ENTRY,  HEADER "Exec=app\n\tExec=/bin/evil\n"                          },
        {MAIN_ENTRY,  HEADER "Exec=app\nExec =/bin/evil\n"                           },
        {MAIN_ENTRY,  HEADER "Exec = app\n"                                          },
        {MAIN_ENTRY,  HEADER "Exec=app\n   \n"                                       },
        {MAIN_ENTRY,  HEADER "Exec=app\nno key\n"                                    },

        /* keys that would escape our checks */
        {MAIN_ENTRY,  HEADER "Exec=app\nExec[de]=/bin/evil\n"                        },
        {MAIN_ENTRY,  HEADER "Exec=app\nTryExec[de]=/bin/evil\n"                     },
        {MAIN_ENTRY,  HEADER "Exec=app\nType[de]=Link\n"                             },
        {MAIN_ENTRY,  HEADER "Exec=app\nMimeType[de]=x-scheme-handler/https;\n"      },
        {MAIN_ENTRY,  HEADER "Exec=app\nName[$e]=$(evil)\n"                          },
        {MAIN_ENTRY,  HEADER "Exec=app\nName[]=x\n"                                  },
        {MAIN_ENTRY,  HEADER "Exec=app\nName[de=x\n"                                 },

        /* malformed files */
        {MAIN_ENTRY,  ""                                                             },
        {MAIN_ENTRY,  "Exec=app\n" HEADER                                            },
        {MAIN_ENTRY,  "[Other]\n" HEADER "Exec=app\n"                                },
        {MAIN_ENTRY,  HEADER "Exec=app\n[]\n"                                        },
        {MAIN_ENTRY,  HEADER "Exec=app\n[a]b]\n"                                     },
        {MAIN_ENTRY,  HEADER "Exec=app\n[a\n"                                        },
        {MAIN_ENTRY,  "[Desktop Entry]\nExec=app\n"                                  },
        {MAIN_ENTRY,  HEADER                                                         },
        {MAIN_ENTRY,  HEADER "[Desktop Action Tool]\nExec=app\n"                     },
        {MAIN_ENTRY,  "[Desktop Entry]\nType=Link\nURL=file:///etc\nExec=app\n"      },
        {MAIN_ENTRY,  HEADER "DBusActivatable=true\nExec=app\n"                      },
        {MAIN_ENTRY,  HEADER "DBusActivatable=1\nExec=app\n"                         },

        /* commands that are not programs of the image */
        {OTHER_ENTRY, HEADER "Exec=\n"                                               },
        {OTHER_ENTRY, HEADER "Exec= app\n"                                           },
        {OTHER_ENTRY, HEADER "Exec=/bin/sh -c evil\n"                                },
        {OTHER_ENTRY, HEADER "Exec=/usr/bin/app\n"                                   },
        {OTHER_ENTRY, HEADER "Exec=../bin/app\n"                                     },
        {OTHER_ENTRY, HEADER "Exec=/app/../bin/sh\n"                                 },
        {OTHER_ENTRY, HEADER "Exec=\"/app/bin/app\"\n"                               },
        {OTHER_ENTRY, HEADER "Exec=%U\n"                                             },
        {OTHER_ENTRY, HEADER "Exec=a\tb\n"                                           },
        {OTHER_ENTRY, HEADER "Exec=-x\n"                                             },

        /* the main entry has to run the application */
        {MAIN_ENTRY,  HEADER "Exec=tool\n"                                           },
        {MAIN_ENTRY,  HEADER "Exec=/app/libexec/app\n"                               },
        {MAIN_ENTRY,  HEADER "Exec=/app/bin/tool\n"                                  },
        {MAIN_ENTRY,  HEADER "Exec=application\n"                                    },

        /* values with a meaning for the host */
        {MAIN_ENTRY,  HEADER "Exec=app\nIcon=/usr/share/pixmaps/x.png\n"             },
        {MAIN_ENTRY,  HEADER "Exec=app\nIcon=../x\n"                                 },
        {MAIN_ENTRY,  HEADER "Exec=app\nMimeType=x-scheme-handler/https;\n"          },
        {MAIN_ENTRY,  HEADER "Exec=app\nMimeType=text/plain;X-Scheme-Handler/HTTP;\n"},
    };

    for (size_t i = 0; i < G_N_ELEMENTS(entries); i++)
        mft_assert_render_refused("desktop-entry", entries[i].path, entries[i].content);
}

#define UNIT_PATH "/app/lib/systemd/system/com.example.app.service"

/**
 * test_units:
 *
 * The commands of a systemd unit are replaced with commands that run the
 * program in the image.
 */
static void
test_units(void)
{
    /* clang-format off */
    static const struct {
        const char *content;
        const char *expected;
    } units[] = {
        /* the commands run in the environment of the component, and nothing else changes */
        {"[Unit]\n"
         "Description=App\n"
         "\n"
         "[Service]\n"
         "Type=oneshot\n"
         "ExecStart=/app/bin/app --report\n"
         "\n"
         "[Install]\n"
         "WantedBy=multi-user.target\n",
         "[Unit]\n"
         "Description=App\n"
         "\n"
         "[Service]\n"
         "Type=oneshot\n"
         "ExecStart=" RUN_APP " --report\n"
         "\n"
         "[Install]\n"
         "WantedBy=multi-user.target\n"},
        {"[Service]\nExecStart=app",
         "[Service]\nExecStart=" RUN_APP "\n"},
        {"[Service]\nExecStart=tool",
         "[Service]\nExecStart=" RUN("tool") "\n"},

        /* every key that runs a command, even if the key is set several times */
        {"[Service]\n"
         "ExecCondition=/app/bin/a\n"
         "ExecStartPre=/app/bin/b\n"
         "ExecStartPre=/app/bin/c\n"
         "ExecStartPost=/app/bin/d\n"
         "ExecReload=/app/bin/e\n"
         "ExecReloadPost=/app/bin/f\n"
         "ExecStop=/app/bin/g\n"
         "ExecStopPost=/app/bin/h\n",
         "[Service]\n"
         "ExecCondition=" RUN("/app/bin/a") "\n"
         "ExecStartPre=" RUN("/app/bin/b") "\n"
         "ExecStartPre=" RUN("/app/bin/c") "\n"
         "ExecStartPost=" RUN("/app/bin/d") "\n"
         "ExecReload=" RUN("/app/bin/e") "\n"
         "ExecReloadPost=" RUN("/app/bin/f") "\n"
         "ExecStop=" RUN("/app/bin/g") "\n"
         "ExecStopPost=" RUN("/app/bin/h") "\n"},
        {"[Socket]\nExecStopPre=/app/bin/a\n",
         "[Socket]\nExecStopPre=" RUN("/app/bin/a") "\n"},

        /* a failure may be ignored, and a key can be cleared */
        {"[Service]\nExecStartPre=-/app/bin/a b\n",
         "[Service]\nExecStartPre=-" RUN("/app/bin/a") " b\n"},
        {"[Service]\nExecStart=-app\n",
         "[Service]\nExecStart=-" RUN_APP "\n"},
        {"[Service]\nExecStart=\nExecStart=app\n",
         "[Service]\nExecStart=\nExecStart=" RUN_APP "\n"},

        /* semicolons that do not separate commands */
        {"[Service]\nExecStart=app a \\; b\n",
         "[Service]\nExecStart=" RUN_APP " a \\; b\n"},
        {"[Service]\nExecStart=app --x=a;b c;\n",
         "[Service]\nExecStart=" RUN_APP " --x=a;b c;\n"},

        /* lines that are not commands are kept unchanged, like comments and unknown groups */
        {"# a\n; b \\\n[X-Foo]\nBar=1\n[Service]\nExecPaths=/app\nExecSearchPath=/app/bin\nNoExecPaths=/tmp\nEnvironment=A=B\n",
         "# a\n; b \\\n[X-Foo]\nBar=1\n[Service]\nExecPaths=/app\nExecSearchPath=/app/bin\nNoExecPaths=/tmp\nEnvironment=A=B\n"},

        /* Alias and Also can name units of the component */
        {"[Install]\nAlias=com.example.app-two.service com.example.app.three.service\nAlso=com.example.app.socket\n",
         "[Install]\nAlias=com.example.app-two.service com.example.app.three.service\nAlso=com.example.app.socket\n"},
        {"[Install]\nAlias=\n",
         "[Install]\nAlias=\n"},
    };
    /* clang-format on */

    for (size_t i = 0; i < G_N_ELEMENTS(units); i++)
        mft_assert_renders("systemd-unit-system", UNIT_PATH, units[i].content, units[i].expected);
}

/**
 * test_units_refused:
 *
 * A systemd unit is refused if we can not be sure what systemd will run for it.
 */
static void
test_units_refused(void)
{
    static const char *const units[] = {
        /* several commands in one key, of which only the first one would run in the image */
        "[Service]\nExecStart=/app/bin/app ; /bin/evil\n",
        "[Service]\nExecStart=/app/bin/app\t;\t/bin/evil\n",
        "[Service]\nExecStart=/app/bin/app  ;  /bin/evil\n",
        "[Service]\nExecStart=/app/bin/app a ;\n",
        "[Service]\nExecStart=/app/bin/app \"a\" ; /bin/evil\n",
        "[Service]\nExecStart=-/app/bin/app ; /bin/evil\n",
        "[Service]\nExecStart=; /bin/evil\n",
        "[Service]\nExecStart=;\n",

        /* prefixes that change how the command runs */
        "[Service]\nExecStart=+/app/bin/app\n",
        "[Service]\nExecStart=!/app/bin/app\n",
        "[Service]\nExecStart=!!/app/bin/app\n",
        "[Service]\nExecStart=:/app/bin/app\n",
        "[Service]\nExecStart=@/app/bin/app argv0\n",
        "[Service]\nExecStart=|/app/bin/app\n",
        "[Service]\nExecStart=-+/app/bin/app\n",
        "[Service]\nExecStart=--/app/bin/app\n",
        "[Service]\nExecStart=- /app/bin/app\n",

        /* commands that are not programs of the image */
        "[Service]\nExecStart=/bin/sh -c evil\n",
        "[Service]\nExecStart= /bin/evil\n",
        "[Service]\nExecStart=/usr/bin/app\n",
        "[Service]\nExecStart=%h/evil\n",
        "[Service]\nExecStart=$EVIL\n",
        "[Service]\nExecStart=\"/app/bin/app\"\n",
        "[Service]\nExecStart=/app/bin/a\tb\n",

        /* keys that may run a command in a version of systemd we do not know */
        "[Service]\nExecFuture=/bin/evil\n",
        "[Service]\nExecStartLater=/bin/evil\n",

        /* lines that we and systemd would see differently */
        "[Service]\nExecStart=/app/bin/app \\\n/bin/evil\n",
        "[Service]\nDescription=x \\\nExecStart=/bin/evil\n",
        /* each line is fine on its own, but joined the semicolon separates two commands */
        "[Service]\nExecStart=/app/bin/app \\\nEnvironment=a ; /bin/evil\n",
        "[Service]\nDescription=x\rExecStartPre=+/bin/sh -c evil\n",
        "\xef\xbb\xbf[Service]\nExecStart=/bin/evil\n",
        "[Service]\n ExecStart=/bin/evil\n",
        "[Service]\n\tExecStart=/bin/evil\n",
        "[Service]\nExecStart =/bin/evil\n",
        "[Service]\nExecStart = /app/bin/app\n",
        "[Service]\nno key\n",
        "ExecStart=/bin/evil\n",
        "[Service\nExecStart=/bin/evil\n",
        "[]\nExecStart=/bin/evil\n",
        "[Service]x\nExecStart=/bin/evil\n",
        "[Service]\nExec[de]=/bin/evil\n",

        /* units that do not belong to the component */
        "[Install]\nAlias=sshd.service\n",
        "[Install]\nAlias=com.example.app-two.service display-manager.service\n",
        "[Install]\nAlso=other.socket\n",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(units); i++)
        mft_assert_render_refused("systemd-unit-system", UNIT_PATH, units[i]);
}

#define MIME_PATH  "/app/share/mime/packages/com.example.app.xml"
#define MIME_XML   "<?xml version='1.0' encoding='UTF-8'?>\n"
#define MIME_NS    "http://www.freedesktop.org/standards/shared-mime-info"
#define MIME_OPEN  "<mime-info xmlns='" MIME_NS "'>"
#define MIME_CLOSE "</mime-info>"
#define MIME_TYPE  "<mime-type type='application/x-app'>"
#define MIME_END   "</mime-type>"

/**
 * test_mime_packages:
 *
 * A MIME package is written out again from the elements that we know.
 */
static void
test_mime_packages(void)
{
    static const struct {
        const char *content;
        const char *expected;
    } packages[] = {
        /* the MIME package of the test application */
        {MIME_XML MIME_OPEN "\n  " MIME_TYPE "\n"
                            "    <comment>App file</comment>\n"
                            "    <icon name='com.example.app'/>\n"
                            "    <glob pattern='*.appfile'/>\n"
                            "  " MIME_END "\n" MIME_CLOSE "\n",
         MIME_XML MIME_OPEN "\n  " MIME_TYPE "\n"
                            "    <comment>App file</comment>\n"
                            "    <icon name='com.example.app'></icon>\n"
                            "    <glob pattern='*.appfile'></glob>\n"
                            "  " MIME_END "\n" MIME_CLOSE "\n"                                             },

        /* the elements that are allowed in a type */
        {MIME_OPEN MIME_TYPE
         "<comment xml:lang='de'>Datei &amp; so</comment>"
         "<acronym>APP</acronym>"
         "<expanded-acronym>App</expanded-acronym>"
         "<generic-icon name='text-x-generic'/>"
         "<sub-class-of type='application/zip'/>"
         "<glob weight='50' pattern='*.appfile' case-sensitive='true'/>"
         "<glob pattern='*.[1-9]~'/>"
         "<magic priority='50'>"
         "<match type='string' offset='0' value='APP'><match type='byte' offset='4' value='1'/></match>"
         "</magic>"
         "<root-XML namespaceURI='urn:app' localName='app'/>" MIME_END MIME_CLOSE,
         MIME_XML MIME_OPEN MIME_TYPE
         "<comment xml:lang='de'>Datei &amp; so</comment>"
         "<acronym>APP</acronym>"
         "<expanded-acronym>App</expanded-acronym>"
         "<generic-icon name='text-x-generic'></generic-icon>"
         "<sub-class-of type='application/zip'></sub-class-of>"
         "<glob weight='50' pattern='*.appfile' case-sensitive='true'></glob>"
         "<glob pattern='*.[1-9]~'></glob>"
         "<magic priority='50'>"
         "<match type='string' offset='0' value='APP'><match type='byte' offset='4' value='1'></match></match>"
         "</magic>"
         "<root-XML namespaceURI='urn:app' localName='app'></root-XML>" MIME_END MIME_CLOSE "\n"           },

        /* content that a different parser could read differently is not passed on */
        {"<?xml version='1.0' encoding='UTF-7'?>" MIME_OPEN MIME_TYPE
         "<comment>+ADw-/comment+AD4APA-glob-deleteall/+AD4APA-comment+AD4-</comment>" MIME_END MIME_CLOSE,
         MIME_XML MIME_OPEN MIME_TYPE
         "<comment>+ADw-/comment+AD4APA-glob-deleteall/+AD4APA-comment+AD4-</comment>" MIME_END MIME_CLOSE
         "\n"                                                                                              },
        {"<?xml version='1.0'?><!DOCTYPE m [<!ENTITY s SYSTEM 'file:///etc/shadow'>]>" MIME_OPEN MIME_TYPE
             MIME_END MIME_CLOSE,
         MIME_XML MIME_OPEN MIME_TYPE MIME_END MIME_CLOSE "\n"                                             },
        {MIME_OPEN "<!-- a -->" MIME_TYPE "<?pi x?>" MIME_END MIME_CLOSE,
         MIME_XML MIME_OPEN MIME_TYPE MIME_END MIME_CLOSE "\n"                                             },
        {MIME_OPEN MIME_TYPE "<comment><![CDATA[<glob-deleteall/>]]></comment>" MIME_END MIME_CLOSE,
         MIME_XML MIME_OPEN MIME_TYPE "<comment>&lt;glob-deleteall/&gt;</comment>" MIME_END MIME_CLOSE "\n"},
    };

    for (size_t i = 0; i < G_N_ELEMENTS(packages); i++) {
        g_autofree char *expected = mft_json(packages[i].expected);

        mft_assert_renders("mime-package", MIME_PATH, packages[i].content, expected);
    }
}

/**
 * test_mime_packages_refused:
 *
 * A MIME package is refused if it could change how the host handles the
 * files of other applications.
 */
static void
test_mime_packages_refused(void)
{
    static const char *const packages[] = {
        /* elements that change MIME types the host already knows */
        MIME_OPEN MIME_TYPE "<glob-deleteall/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<magic-deleteall/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<alias type='application/pdf'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<treemagic><treematch path='x'/></treemagic>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<magic><glob-deleteall/></magic>" MIME_END MIME_CLOSE,

        /* namespace prefixes, which other parsers resolve */
        "<x:mime-info xmlns:x='" MIME_NS "'>" MIME_TYPE MIME_END "</x:mime-info>",
        MIME_OPEN MIME_TYPE "<x:glob-deleteall xmlns:x='" MIME_NS "'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<x:glob-deleteall/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob x:pattern='*.abc'/>" MIME_END MIME_CLOSE,

        /* malformed documents */
        "",
        "not xml",
        MIME_TYPE MIME_END,
        MIME_OPEN MIME_CLOSE,
        "<mime-info>" MIME_TYPE MIME_END MIME_CLOSE,
        "<mime-info xmlns='urn:other'>" MIME_TYPE MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE MIME_OPEN MIME_CLOSE MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE MIME_TYPE MIME_END MIME_END MIME_CLOSE,
        MIME_OPEN "<glob pattern='*.abc'/>" MIME_TYPE MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE,
        MIME_OPEN MIME_TYPE MIME_END MIME_CLOSE "<x/>",
        MIME_OPEN MIME_TYPE "&undefined;" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<comment xmlns='urn:other'/>" MIME_END MIME_CLOSE,

        /* types that are not plain, or that do not belong to an application */
        MIME_OPEN "<mime-type/>" MIME_CLOSE,
        MIME_OPEN "<mime-type type=''/>" MIME_CLOSE,
        MIME_OPEN "<mime-type type='application'/>" MIME_CLOSE,
        MIME_OPEN "<mime-type type='../../../evil'/>" MIME_CLOSE,
        MIME_OPEN "<mime-type type='application/../../evil'/>" MIME_CLOSE,
        MIME_OPEN "<mime-type type='application/..'/>" MIME_CLOSE,
        MIME_OPEN "<mime-type type='application/.x'/>" MIME_CLOSE,
        MIME_OPEN "<mime-type type='x-scheme-handler/https'/>" MIME_CLOSE,
        MIME_OPEN "<mime-type type='inode/directory'/>" MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<sub-class-of type='../x'/>" MIME_END MIME_CLOSE,

        /* globs that match the files of other types */
        MIME_OPEN MIME_TYPE "<glob pattern='*'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*.*'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='?*'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*a'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*:xx'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*.pdf:cs'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*.a b'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*.abc&#10;90:text/plain:*'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*.abc' weight='51'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*.abc' weight='x'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<glob pattern='*.abc' weight='-1'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<magic priority='100'/>" MIME_END MIME_CLOSE,

        /* the MIME database holds root-XML values as words on one line */
        MIME_OPEN MIME_TYPE "<root-XML namespaceURI='a b' localName='c'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<root-XML namespaceURI='a' localName='c&#10;d'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<root-XML namespaceURI='' localName='c'/>" MIME_END MIME_CLOSE,

        /* icons that are not plain names */
        MIME_OPEN MIME_TYPE "<icon name='/etc/passwd'/>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<generic-icon name='../x'/>" MIME_END MIME_CLOSE,

        /* text with control characters or invalid UTF-8 */
        MIME_OPEN MIME_TYPE "<comment>\r</comment>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<comment>\xff</comment>" MIME_END MIME_CLOSE,
    };

    for (size_t i = 0; i < G_N_ELEMENTS(packages); i++)
        mft_assert_render_refused("mime-package", MIME_PATH, packages[i]);
}

#define RULE_PATH "/app/lib/udev/rules.d/60-com.example.app.rules"

/**
 * test_udev_rules:
 *
 * A udev rule file is installed unchanged if its rules only act on the matched device.
 */
static void
test_udev_rules(void)
{
    static const char *const rules[] = {
        /* the rules of the test application, and rules that instruments usually need */
        "# a comment \\\n\nSUBSYSTEM==\"mem\", KERNEL==\"null\", ENV{MFTESTAPP}=\"1\"\n",
        "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1234\", ATTRS{idProduct}!=\"5678\", MODE=\"0660\", GROUP=\"plugdev\", TAG+=\"uaccess\"\n",
        "KERNEL==\"ttyUSB[0-9]*\", SYMLINK+=\"app%n\", OWNER=\"root\"\n",
        "ACTION==\"remove\", GOTO=\"app_end\"\nSUBSYSTEM!=\"usb\", GOTO=\"app_end\"\nLABEL=\"app_end\"\n",
        "SUBSYSTEM==\"usb\", IMPORT{builtin}=\"usb_id\", IMPORT{parent}=\"ID_*\", ATTR{power/control}=\"on\"\n",
        "ATTR{[dmi/id]sys_vendor}==\"Example\", SYSCTL{kernel/hostname}==\"lab\", TEST{0644}==\"x\"\n",
        "  KERNEL==\"a\" MODE=\"0600\",,TAG+=\"b\"\n",

        /* units of the component */
        "TAG+=\"systemd\", ENV{SYSTEMD_WANTS}+=\"com.example.app.service\"\n",
        "ENV{SYSTEMD_WANTS}=\"com.example.app-a.service com.example.app-b.target\"\n",
        "ENV{SYSTEMD_WANTS}+=\"com.example.app@%k.service\"\n",
        "ENV{SYSTEMD_WANTS}+=\"com.example.app-worker@$kernel.service\"\n",
        "ENV{SYSTEMD_USER_WANTS}+=\"com.example.app.service\"\n",
        "ENV{SYSTEMD_WANTS}=\"\"\n",

        /* comparing a value does not change or start anything */
        "ENV{SYSTEMD_WANTS}==\"other.service\"\n",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(rules); i++)
        mft_assert_renders("udev-rule", RULE_PATH, rules[i], rules[i]);
}

/**
 * test_udev_rules_refused:
 *
 * A udev rule file is refused if a rule runs a program, or if we can not be
 * sure how udev will read it.
 */
static void
test_udev_rules_refused(void)
{
    static const char *const rules[] = {
        /* rules that run programs */
        "RUN+=\"/bin/evil\"\n",
        "RUN=\"/bin/evil\"\n",
        "RUN:=\"/bin/evil\"\n",
        "RUN{program}+=\"/bin/evil\"\n",
        "RUN{builtin}+=\"kmod load evil\"\n",
        "PROGRAM==\"/bin/evil\"\n",
        "PROGRAM=\"/bin/evil\"\n",
        "PROGRAM!=\"/bin/evil\"\n",
        "IMPORT{program}=\"/bin/evil\"\n",
        "IMPORT{program}==\"/bin/evil\"\n",
        "KERNEL==\"a\", MODE=\"0600\", RUN+=\"/bin/evil\"\n",
        "KERNEL==\"a\" RUN+=\"/bin/evil\"\n",
        "KERNEL==\"a\",RUN+=\"/bin/evil\"\n",
        "KERNEL==\"a\"RUN+=\"/bin/evil\"\n",
        "\tRUN+=\"/bin/evil\"\n",

        /* rules that change something other than the matched device */
        "SYSCTL{kernel/core_pattern}=\"|/bin/evil\"\n",
        "ATTR{[block/sda]size}=\"1\"\n",
        "ATTR{$env{X}}=\"1\"\n",
        "ATTR{%k}=\"1\"\n",

        /* units that do not belong to the component */
        "ENV{SYSTEMD_WANTS}+=\"sshd.service\"\n",
        "ENV{SYSTEMD_WANTS}=\"com.example.app.service other.service\"\n",
        "ENV{SYSTEMD_USER_WANTS}+=\"other.service\"\n",
        "ENV{SYSTEMD_WANTS}+=\"$env{X}\"\n",
        "ENV{SYSTEMD_WANTS}+=\"%E{X}.service\"\n",
        "ENV{SYSTEMD_WANTS}+=\"other@com.example.app.service\"\n",
        "ENV{SYSTEMD_WANTS}+=\"com.example.app.mount\"\n",

        /* values that udev could end at a different quote than we do */
        "KERNEL==\"a\\\", RUN+=\"/bin/evil\"\n",
        "KERNEL==\"a\\\\\", RUN+=\"/bin/evil\"\n",
        "KERNEL==e\"a\\\\\", MODE=\"0666\"\n",
        "KERNEL==i\"a\"\n",
        "KERNEL==ei\"a\"\n",
        "KERNEL==\"a\\\"\n",
        "MODE=0666\n",
        "MODE=\"0666\n",

        /* lines that we and udev would see differently */
        "KERNEL==\"a\", \\\nRUN+=\"/bin/evil\"\n",
        "KERNEL==\"a\", \\\n# comment\nMODE=\"0600\"\n",
        "KERNEL==\"a\"\rRUN+=\"/bin/evil\"\n",
        "# a comment\rRUN+=\"/bin/evil\"\n",
        "\xef\xbb\xbfKERNEL==\"a\"\n",

        /* lines that are not plain rules */
        "KERNEL == \"a\"\n",
        "KERNEL= =\"a\"\n",
        "KERNEL\n",
        "kernel==\"a\"\n",
        "ATTR{a==\"b\"\n",
        "ATTR{a b}==\"c\"\n",
        "ATTR{a\"b}==\"c\"\n",
        "ENV{\"SYSTEMD_WANTS\"}+=\"sshd.service\"\n",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(rules); i++)
        mft_assert_render_refused("udev-rule", RULE_PATH, rules[i]);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/exports/kinds", test_kinds);
    g_test_add_func("/exports/section", test_section);
    g_test_add_func("/exports/malformed-section", test_malformed_section);
    g_test_add_func("/exports/export-paths", test_export_paths);
    g_test_add_func("/exports/privileged", test_privileged);
    g_test_add_func("/exports/diff-privileged", test_diff_privileged);
    g_test_add_func("/exports/check-section", test_check_section);
    g_test_add_func("/exports/check-component", test_check_component);
    g_test_add_func("/exports/check-file-paths", test_check_file_paths);
    g_test_add_func("/exports/check-aliases", test_check_aliases);
    g_test_add_func("/exports/render-aliases", test_render_aliases);
    g_test_add_func("/exports/desktop-entries", test_desktop_entries);
    g_test_add_func("/exports/desktop-entries-refused", test_desktop_entries_refused);
    g_test_add_func("/exports/units", test_units);
    g_test_add_func("/exports/units-refused", test_units_refused);
    g_test_add_func("/exports/mime-packages", test_mime_packages);
    g_test_add_func("/exports/mime-packages-refused", test_mime_packages_refused);
    g_test_add_func("/exports/udev-rules", test_udev_rules);
    g_test_add_func("/exports/udev-rules-refused", test_udev_rules_refused);

    return g_test_run();
}
