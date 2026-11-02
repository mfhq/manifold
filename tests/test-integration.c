/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>
#include <manifold.h>

#include "mft-utils.h"

#define SHA256_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define BLAKE3_A "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define HASHES   "\"sha256\": \"" SHA256_A "\", \"blake3\": \"" BLAKE3_A "\""

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
        .integration = integration,
    };
}

/**
 * mft_manifest_new:
 * @integration: (nullable): the integration section as JSON
 *
 * Returns: (transfer full) (nullable): the parsed manifest of an application
 */
static MfManifest *
mft_manifest_new(const char *integration, GError **error)
{
    MftTestManifest m = mft_app_manifest(integration);
    g_autofree char *json = mft_manifest_json(&m);

    return mf_manifest_new_from_data(json, -1, error);
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
    g_autofree char *json = mft_manifest_json(m);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) manifest = mf_manifest_new_from_data(json, -1, &error);

    g_assert_no_error(error);
    if (mf_manifest_check_integration(manifest, &error))
        return TRUE;
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);

    return FALSE;
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
test_privileged(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) rule = mft_manifest_new(
        "{\"udev-rule\": [{\"path\": \"/app/lib/udev/rules.d/60-com.example.app.rules\", " HASHES "}]}",
        &error);
    g_autoptr(MfManifest) unit = NULL;

    g_assert_no_error(error);
    g_assert_true(mf_integration_is_privileged(mf_manifest_get_integration(rule)));

    /* a user unit only runs as a user */
    unit = mft_manifest_new(
        "{\"systemd-unit-user\": [{\"path\": \"/app/lib/systemd/user/com.example.app.service\", " HASHES
        "}]}",
        &error);
    g_assert_no_error(error);
    g_assert_false(mf_integration_is_privileged(mf_manifest_get_integration(unit)));

    g_assert_true(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM));
    g_assert_true(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_UDEV_RULE));
    g_assert_false(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER));
    g_assert_false(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_DESKTOP_ENTRY));
    g_assert_false(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_UNKNOWN));
    g_assert_false(mf_integration_kind_is_privileged(MF_INTEGRATION_KIND_LAST));
}

static void
test_section(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) manifest = mft_manifest_new(
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
    g_assert_cmpstr(mf_integration_alias_get_export_path(alias), ==, "bin/app");

    /* the value of a kind we do not know is not looked at */
    unknown = mf_integration_get_unknown_kinds(integration);
    g_assert_cmpuint(unknown->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(unknown, 0), ==, "from-the-future");
}

static void
test_no_section(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) without = mft_manifest_new(NULL, &error);
    g_autoptr(MfManifest) null = mft_manifest_new("null", &error);
    g_autoptr(MfManifest) empty = mft_manifest_new("{}", &error);

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
        g_autoptr(MfManifest) manifest = mft_manifest_new(sections[i], &error);

        g_test_message("%s", sections[i]);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(manifest);
    }
}

static void
test_check_file_paths(void)
{
    static const struct {
        const char *kind;
        const char *path;
        gboolean valid;
    } files[] = {
        {"desktop-entry",       "/app/share/applications/com.example.app.desktop",                     TRUE },
        {"desktop-entry",       "/app/share/applications/com.example.app.open.desktop",                TRUE },
        {"desktop-entry",       "/app/share/applications/com.example.app-open.desktop",                TRUE },
        {"icons",               "/app/share/icons/hicolor/scalable/apps/com.example.app.svg",          TRUE },
        {"icons",               "/app/share/icons/hicolor/48x48/apps/com.example.app.png",             TRUE },
        {"icons",               "/app/share/icons/hicolor/48x48/apps/com.example.app.jxl",             TRUE },
        {"icons",               "/app/share/icons/hicolor/16x16@2x/apps/com.example.app.png",          TRUE },
        {"icons",               "/app/share/icons/hicolor/symbolic/apps/com.example.app-symbolic.svg", TRUE },
        {"icons",               "/app/share/icons/hicolor/48x48/mimetypes/com.example.app.doc.png",    TRUE },
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

        /* not where the kind belongs */
        {"desktop-entry",       "/usr/share/applications/com.example.app.desktop",                     FALSE},
        {"desktop-entry",       "share/applications/com.example.app.desktop",                          FALSE},
        {"desktop-entry",       "/app/share/applications/kde/com.example.app.desktop",                 FALSE},
        {"desktop-entry",       "/app/share/applications//com.example.app.desktop",                    FALSE},
        {"desktop-entry",       "/app/share/applications/../applications/com.example.app.desktop",     FALSE},
        {"desktop-entry",       "/app/share/applications/../../../etc/com.example.app.desktop",        FALSE},
        {"systemd-unit-user",   "/app/lib/systemd/system/com.example.app.service",                     FALSE},
        {"systemd-unit-system", "/app/etc/systemd/system/com.example.app.service",                     FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.service.d/x.conf",            FALSE},
        {"icons",               "/app/share/icons/Adwaita/48x48/apps/com.example.app.png",             FALSE},
        {"icons",               "/app/share/icons/hicolor/apps/com.example.app.png",                   FALSE},
        {"icons",               "/app/share/icons/hicolor/big/apps/com.example.app.png",               FALSE},
        {"icons",               "/app/share/icons/hicolor/48x48/actions/com.example.app.png",          FALSE},
        {"icons",               "/app/share/icons/hicolor/48x48/apps/x/com.example.app.png",           FALSE},
        {"icons",               "/app/share/icons/hicolor/48x48\\n/apps/com.example.app.png",          FALSE},

        /* not named after the component */
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

        /* not a name the host picks up */
        {"desktop-entry",       "/app/share/applications/com.example.app",                             FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app.desktop.bak",                 FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app@.desktop",                    FALSE},
        {"icons",               "/app/share/icons/hicolor/48x48/apps/com.example.app.gif",             FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.mount",                       FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.automount",                   FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.swap",                        FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.device",                      FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app.slice",                       FALSE},
        {"systemd-unit-system", "/app/lib/systemd/system/com.example.app@one.service",                 FALSE},
        {"udev-rule",           "/app/lib/udev/rules.d/com.example.app.rules",                         FALSE},
        {"udev-rule",           "/app/lib/udev/rules.d/6-com.example.app.rules",                       FALSE},
        {"udev-rule",           "/app/lib/udev/rules.d/600-com.example.app.rules",                     FALSE},

        /* not a safe name */
        {"desktop-entry",       "/app/share/applications/com.example.app .desktop",                    FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app.a'b.desktop",                 FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app.a\\nb.desktop",               FALSE},
        {"desktop-entry",       "/app/share/applications/com.example.app.a\\u001bb.desktop",           FALSE},
    };

    for (gsize i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree char *section = g_strdup_printf("{\"%s\": [{\"path\": \"%s\", " HASHES "}]}",
                                                   files[i].kind,
                                                   files[i].path);
        MftTestManifest m = mft_app_manifest(section);

        g_test_message("%s %s", files[i].kind, files[i].path);
        g_assert_cmpint(mft_passes_check(&m), ==, files[i].valid);
    }
}

static void
test_check_aliases(void)
{
    static const struct {
        const char *name;
        const char *command;
        gboolean valid;
    } aliases[] = {
        {"app",       "/app/bin/app",          TRUE },
        {"app-cli.1", "/app/bin/app",          TRUE },
        {"g++",       "/app/libexec/app/g++",  TRUE },

        /* not a safe name */
        {"",          "/app/bin/app",          FALSE},
        {".app",      "/app/bin/app",          FALSE},
        {"-app",      "/app/bin/app",          FALSE},
        {"a/b",       "/app/bin/app",          FALSE},
        {"a b",       "/app/bin/app",          FALSE},
        {"a'b",       "/app/bin/app",          FALSE},
        {"a;b",       "/app/bin/app",          FALSE},
        {"a\\nb",     "/app/bin/app",          FALSE},
        {"$(id)",     "/app/bin/app",          FALSE},

        /* reserved names of tools that a host may lack */
        {"sudo",      "/app/bin/app",          FALSE},
        {"pkexec",    "/app/bin/app",          FALSE},
        {"run0",      "/app/bin/app",          FALSE},
        {"manifold",  "/app/bin/app",          FALSE},
        {"mfrun",     "/app/bin/app",          FALSE},

        /* not a program of the image, or not one that is safe to put in a script */
        {"app",       "",                      FALSE},
        {"app",       "app",                   FALSE},
        {"app",       "bin/app",               FALSE},
        {"app",       "/usr/bin/app",          FALSE},
        {"app",       "/app",                  FALSE},
        {"app",       "/app/",                 FALSE},
        {"app",       "/app/bin/",             FALSE},
        {"app",       "/app//bin/app",         FALSE},
        {"app",       "/app/../bin/sh",        FALSE},
        {"app",       "/app/bin/../../bin/sh", FALSE},
        {"app",       "/app/bin/.hidden",      FALSE},
        {"app",       "/app/bin/a b",          FALSE},
        {"app",       "/app/bin/a'b",          FALSE},
        {"app",       "/app/bin/a\\nb",        FALSE},
        {"app",       "/app/bin/$(id)",        FALSE},
    };

    for (gsize i = 0; i < G_N_ELEMENTS(aliases); i++) {
        g_autofree char *section = g_strdup_printf("{\"alias\": [{\"name\": \"%s\", \"command\": \"%s\"}]}",
                                                   aliases[i].name,
                                                   aliases[i].command);
        MftTestManifest m = mft_app_manifest(section);

        g_test_message("%s %s", aliases[i].name, aliases[i].command);
        g_assert_cmpint(mft_passes_check(&m), ==, aliases[i].valid);
    }
}

static void
test_render_aliases(void)
{
    static const struct {
        const char *command;
        const char *expected;
    } aliases[] = {
        /* the program of the application is what mfrun runs by default */
        {"/app/bin/app",      "#!/bin/sh\nexec " MF_MFRUN_PATH " com.example.app \"$@\"\n"       },
        {"/app/libexec/tool",
         "#!/bin/sh\nexec " MF_MFRUN_PATH " --command=/app/libexec/tool com.example.app \"$@\"\n"},

        /* the script ends up in the PATH, so we check again what goes into it */
        {"/usr/bin/sh",       NULL                                                               },
        {"/app/bin/$(id)",    NULL                                                               },
    };

    for (gsize i = 0; i < G_N_ELEMENTS(aliases); i++) {
        g_autofree char *section = g_strdup_printf("{\"alias\": [{\"name\": \"tool\", \"command\": \"%s\"}]}",
                                                   aliases[i].command);
        g_autoptr(GError) error = NULL;
        g_autoptr(MfManifest) manifest = mft_manifest_new(section, &error);
        g_autoptr(GBytes) script = NULL;
        g_autofree char *text = NULL;
        gsize size;
        const char *data;

        g_assert_no_error(error);
        script = mf_integration_alias_render(
            g_ptr_array_index(mf_integration_get_aliases(mf_manifest_get_integration(manifest)), 0),
            manifest,
            &error);
        if (aliases[i].expected == NULL) {
            g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
            g_assert_null(script);
            continue;
        }
        g_assert_no_error(error);
        data = g_bytes_get_data(script, &size);
        text = g_strndup(data, size);
        g_assert_cmpstr(text, ==, aliases[i].expected);
    }
}

static void
test_check_section(void)
{
    MftTestManifest m = mft_app_manifest("{\"from-the-future\": []}");

    /* a kind we do not know must not be dropped silently */
    g_assert_false(mft_passes_check(&m));

    m.integration =
        "{\"alias\": [{\"name\": \"app\", \"command\": \"/app/bin/a\"},"
        " {\"name\": \"app\", \"command\": \"/app/bin/b\"}]}";
    g_assert_false(mft_passes_check(&m));

    m.integration = "{\"mime-package\": [{\"path\": \"/app/share/mime/packages/com.example.app.xml\", " HASHES
                    "},"
                    " {\"path\": \"/app/share/mime/packages/com.example.app.xml\", " HASHES "}]}";
    g_assert_false(mft_passes_check(&m));

    m.integration = NULL;
    g_assert_true(mft_passes_check(&m));
    m.integration = "{}";
    g_assert_true(mft_passes_check(&m));
}

static void
test_check_component(void)
{
    static const char *const refused_ids[] = {"ssh", "com.example", "com..example", "com.example."};
    static const char *const refused_kinds[] = {"kit", "platform", "plugin"};
    MftTestManifest m = mft_app_manifest("{}");

    for (gsize i = 0; i < G_N_ELEMENTS(refused_ids); i++) {
        m.component_id = refused_ids[i];
        g_test_message("%s", refused_ids[i]);
        g_assert_false(mft_passes_check(&m));
    }

    m = mft_app_manifest("{}");
    for (gsize i = 0; i < G_N_ELEMENTS(refused_kinds); i++) {
        m.kind = refused_kinds[i];
        g_test_message("%s", refused_kinds[i]);
        g_assert_false(mft_passes_check(&m));
    }

    m = mft_app_manifest("{}");
    m.sdk = TRUE;
    g_assert_false(mft_passes_check(&m));

    /* an SDK without an integration section is fine */
    m.integration = NULL;
    g_assert_true(mft_passes_check(&m));
}

static void
test_manifest_messages(void)
{
    MftTestManifest manifests[] = {
        mft_app_manifest(NULL),
        mft_app_manifest(NULL),
        mft_app_manifest(NULL),
    };

    manifests[0].kind = "\\u001b[2J";
    manifests[1].command = "\\u001b[2J";
    manifests[2].architecture = "\\u001b[2J";

    /* a manifest must not be able to control the terminal of the admin */
    for (gsize i = 0; i < G_N_ELEMENTS(manifests); i++) {
        g_autofree char *json = mft_manifest_json(&manifests[i]);
        g_autoptr(GError) error = NULL;
        g_autoptr(MfManifest) manifest = mf_manifest_new_from_data(json, -1, &error);

        g_assert_null(manifest);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_test_message("%s", error->message);
        g_assert_null(strchr(error->message, '\033'));
    }
}

static void
test_check_messages(void)
{
    static const char *const sections[] = {
        "{\"desktop-entry\": [{\"path\": \"/tmp/\\u001b[2Jx\", " HASHES "}]}",
        "{\"desktop-entry\": [{\"path\": \"/app/share/applications/\\u001b[2Jx\", " HASHES "}]}",
        "{\"alias\": [{\"name\": \"\\u001b[2J\", \"command\": \"/app/bin/app\"}]}",
        "{\"alias\": [{\"name\": \"app\", \"command\": \"\\u001b[2J\"}]}",
        "{\"\\u001b[2J\": []}",
    };

    /* a manifest must not be able to control the terminal of the admin */
    for (gsize i = 0; i < G_N_ELEMENTS(sections); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(MfManifest) manifest = mft_manifest_new(sections[i], &error);

        g_assert_no_error(error);
        g_assert_false(mf_manifest_check_integration(manifest, &error));
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_test_message("%s", error->message);
        g_assert_null(strchr(error->message, '\033'));
    }
}

/**
 * mft_render:
 * @kind: the key of the kind of the file
 * @path: where the file is in the image
 * @content: what the file holds
 *
 * Returns: (transfer full) (nullable): the file to install
 */
static GBytes *
mft_render(const char *kind, const char *path, GBytes *content, GError **error)
{
    g_autofree char *section = g_strdup_printf("{\"%s\": [{\"path\": \"%s\", " HASHES "}]}", kind, path);
    g_autoptr(GError) local_error = NULL;
    g_autoptr(MfManifest) manifest = mft_manifest_new(section, &local_error);
    MfIntegrationFile *file;

    g_assert_no_error(local_error);
    g_assert_true(mf_manifest_check_integration(manifest, &local_error));
    file = g_ptr_array_index(mf_integration_get_files(mf_manifest_get_integration(manifest)), 0);

    return mf_integration_file_render(file, manifest, content, error);
}

/**
 * mft_render_desktop_entry:
 * @name: the file name of the desktop entry
 * @content: what the desktop entry holds
 *
 * Returns: (transfer full) (nullable): the desktop entry to install
 */
static GBytes *
mft_render_desktop_entry(const char *name, GBytes *content, GError **error)
{
    g_autofree char *path = g_strconcat("/app/share/applications/", name, NULL);

    return mft_render("desktop-entry", path, content, error);
}

#define MAIN_ENTRY  "com.example.app.desktop"
#define OTHER_ENTRY "com.example.app.open.desktop"
#define HEADER      "[Desktop Entry]\nType=Application\nName=App\n"

static void
test_desktop_entries(void)
{
    static const struct {
        const char *name;
        const char *content;
        const char *expected;
    } entries[] = {
        /* the main entry runs the command of the application, however it names it */
        {MAIN_ENTRY,  HEADER "Exec=app\n",                                                                                      HEADER "Exec=" MF_MFRUN_PATH " com.example.app\n"   },
        {MAIN_ENTRY,  HEADER "Exec=/app/bin/app %U\n",                                                                          HEADER "Exec=" MF_MFRUN_PATH " com.example.app %U\n"},
        {MAIN_ENTRY,
         HEADER "Exec=app --flag \"a b\" %f\n",
         HEADER "Exec=" MF_MFRUN_PATH " com.example.app --flag \"a b\" %f\n"                                                                                                        },

        /* other entries can run the command of the application, or any other program of the image */
        {OTHER_ENTRY,
         HEADER "Exec=app --open %f\n",
         HEADER "Exec=" MF_MFRUN_PATH " com.example.app --open %f\n"                                                                                                                },
        {OTHER_ENTRY,
         HEADER "Exec=/app/libexec/tool\n",
         HEADER "Exec=" MF_MFRUN_PATH " --command=/app/libexec/tool com.example.app\n"                                                                                              },
        {OTHER_ENTRY,
         HEADER "Exec=tool %u\n",
         HEADER "Exec=" MF_MFRUN_PATH " --command=tool com.example.app %u\n"                                                                                                        },

        /* the actions of the main entry can run any program of the image */
        {MAIN_ENTRY,
         HEADER "Exec=app\nActions=Tool;\n\n[Desktop Action Tool]\nName=Tool\nExec=tool\n",
         HEADER "Exec=" MF_MFRUN_PATH
                " com.example.app\nActions=Tool;\n\n[Desktop Action Tool]\nName=Tool\nExec=" MF_MFRUN_PATH
                " --command=tool com.example.app\n"                                                                                                                                 },

        /* all other lines stay as they are, and the rendered file always ends in a newline */
        {MAIN_ENTRY,
         "# a comment\n\n[Desktop Entry]\nType=Application\nName=App\nName[de_DE.UTF-8@euro]=Äpp\n"
         "X-KDE-Something=1\nExec=app\nIcon=com.example.app\nMimeType=application/x-app;x-scheme-handler/app;\nTerminal=false",
         "# a comment\n\n[Desktop Entry]\nType=Application\nName=App\nName[de_DE.UTF-8@euro]=Äpp\n"
         "X-KDE-Something=1\nExec=" MF_MFRUN_PATH
         " com.example.app\nIcon=com.example.app\nMimeType=application/x-app;x-scheme-handler/app;\nTerminal=false\n"                                                               },

        /* the program of TryExec is in the image, and not on the host */
        {MAIN_ENTRY,  HEADER "TryExec=app\nExec=app\n",                                                                         HEADER "Exec=" MF_MFRUN_PATH " com.example.app\n"   },
        {MAIN_ENTRY,
         HEADER "DBusActivatable=false\nExec=app\n",
         HEADER "DBusActivatable=false\nExec=" MF_MFRUN_PATH " com.example.app\n"                                                                                                   },
    };

    for (gsize i = 0; i < G_N_ELEMENTS(entries); i++) {
        g_autoptr(GBytes) content = g_bytes_new_static(entries[i].content, strlen(entries[i].content));
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) rendered = NULL;
        g_autofree char *text = NULL;
        gsize size;
        const char *data;

        g_test_message("%s", entries[i].content);
        rendered = mft_render_desktop_entry(entries[i].name, content, &error);
        g_assert_no_error(error);
        data = g_bytes_get_data(rendered, &size);
        text = g_strndup(data, size);
        g_assert_cmpstr(text, ==, entries[i].expected);
    }
}

static void
test_desktop_entries_refused(void)
{
#define ENTRY(name, text) {name, text, sizeof(text) - 1}

    static const struct {
        const char *name;
        const char *content;
        gsize size;
    } entries[] = {
        /* lines that other parsers see differently */
        ENTRY(MAIN_ENTRY, HEADER "Comment=x\rExec=/bin/evil\nExec=app\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\n\0Exec=/bin/evil\n"),
        ENTRY(MAIN_ENTRY, HEADER "Comment=\xff\xfe\nExec=app\n"),
        ENTRY(MAIN_ENTRY, "\xef\xbb\xbf" HEADER "Exec=app\n"),
        ENTRY(MAIN_ENTRY, HEADER "Comment=x\\\nExec=app\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nExec=/bin/evil\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\n[Desktop Entry]\nExec=/bin/evil\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\n Exec=/bin/evil\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\n\tExec=/bin/evil\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nExec =/bin/evil\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec = app\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\n   \n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nno key\n"),

        /* keys that would escape our checks */
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nExec[de]=/bin/evil\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nTryExec[de]=/bin/evil\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nType[de]=Link\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nMimeType[de]=x-scheme-handler/https;\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nName[$e]=$(evil)\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nName[]=x\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nName[de=x\n"),

        /* the shape of the file */
        ENTRY(MAIN_ENTRY, ""),
        ENTRY(MAIN_ENTRY, "Exec=app\n" HEADER),
        ENTRY(MAIN_ENTRY, "[Other]\n" HEADER "Exec=app\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\n[]\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\n[a]b]\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\n[a\n"),
        ENTRY(MAIN_ENTRY, "[Desktop Entry]\nExec=app\n"),
        ENTRY(MAIN_ENTRY, HEADER),
        ENTRY(MAIN_ENTRY, HEADER "[Desktop Action Tool]\nExec=app\n"),
        ENTRY(MAIN_ENTRY, "[Desktop Entry]\nType=Link\nURL=file:///etc\nExec=app\n"),
        ENTRY(MAIN_ENTRY, HEADER "DBusActivatable=true\nExec=app\n"),
        ENTRY(MAIN_ENTRY, HEADER "DBusActivatable=1\nExec=app\n"),

        /* commands that are not programs of the image */
        ENTRY(OTHER_ENTRY, HEADER "Exec=\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec= app\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec=/bin/sh -c evil\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec=/usr/bin/app\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec=../bin/app\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec=/app/../bin/sh\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec=\"/app/bin/app\"\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec=%U\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec=a\tb\n"),
        ENTRY(OTHER_ENTRY, HEADER "Exec=-x\n"),

        /* the main entry has to run the application */
        ENTRY(MAIN_ENTRY, HEADER "Exec=tool\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=/app/libexec/app\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=/app/bin/tool\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=application\n"),

        /* values with a meaning for the host */
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nIcon=/usr/share/pixmaps/x.png\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nIcon=../x\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nMimeType=x-scheme-handler/https;\n"),
        ENTRY(MAIN_ENTRY, HEADER "Exec=app\nMimeType=text/plain;X-Scheme-Handler/HTTP;\n"),
    };

#undef ENTRY

    for (gsize i = 0; i < G_N_ELEMENTS(entries); i++) {
        g_autoptr(GBytes) content = g_bytes_new_static(entries[i].content, entries[i].size);
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) rendered = NULL;

        g_test_message("%s", entries[i].content);
        rendered = mft_render_desktop_entry(entries[i].name, content, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(rendered);
    }
}

static void
test_text_size_limit(void)
{
    gsize limit = 1024 * 1024;
    const char *text = HEADER "Exec=app\n#";
    gsize used = strlen(text);
    g_autofree char *largest = g_malloc(limit + 1);
    g_autoptr(GError) error = NULL;

    memcpy(largest, text, used);
    memset(largest + used, 'x', limit - used);
    largest[limit - 1] = '\n';
    largest[limit] = '\n';

    {
        g_autoptr(GBytes) fits = g_bytes_new_static(largest, limit);
        g_autoptr(GBytes) rendered = mft_render_desktop_entry(MAIN_ENTRY, fits, &error);

        g_assert_no_error(error);
        g_assert_nonnull(rendered);
    }
    {
        g_autoptr(GBytes) too_large = g_bytes_new_static(largest, limit + 1);
        g_autoptr(GBytes) rendered = mft_render_desktop_entry(MAIN_ENTRY, too_large, &error);

        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(rendered);
    }
}

#define UNIT_PATH "/app/lib/systemd/system/com.example.app.service"

static void
test_units(void)
{
    static const struct {
        const char *content;
        const char *expected;
    } units[] = {
        /* the commands run in the environment of the component, and nothing else changes */
        {"[Unit]\nDescription=App\n\n[Service]\nType=oneshot\nExecStart=/app/bin/app --report\n\n"
         "[Install]\nWantedBy=multi-user.target\n",                                                                     "[Unit]\nDescription=App\n\n[Service]\nType=oneshot\nExecStart=" MF_MFRUN_PATH
         " com.example.app --report\n\n"
         "[Install]\nWantedBy=multi-user.target\n"                                                                              },
        {"[Service]\nExecStart=app",                                                                                             "[Service]\nExecStart=" MF_MFRUN_PATH " com.example.app\n" },
        {"[Service]\nExecStart=tool",
         "[Service]\nExecStart=" MF_MFRUN_PATH " --command=tool com.example.app\n"                                                                                                          },

        /* every key that runs something, however often it is there */
        {"[Service]\nExecCondition=/app/bin/a\nExecStartPre=/app/bin/b\nExecStartPre=/app/bin/c\n"
         "ExecStartPost=/app/bin/d\nExecReload=/app/bin/e\nExecReloadPost=/app/bin/f\n"
         "ExecStop=/app/bin/g\nExecStopPost=/app/bin/h\n",                                                              "[Service]\nExecCondition=" MF_MFRUN_PATH " --command=/app/bin/a com.example.app\n"
         "ExecStartPre=" MF_MFRUN_PATH " --command=/app/bin/b com.example.app\n"
         "ExecStartPre=" MF_MFRUN_PATH " --command=/app/bin/c com.example.app\n"
         "ExecStartPost=" MF_MFRUN_PATH " --command=/app/bin/d com.example.app\n"
         "ExecReload=" MF_MFRUN_PATH " --command=/app/bin/e com.example.app\n"
         "ExecReloadPost=" MF_MFRUN_PATH " --command=/app/bin/f com.example.app\n"
         "ExecStop=" MF_MFRUN_PATH " --command=/app/bin/g com.example.app\n"
         "ExecStopPost=" MF_MFRUN_PATH " --command=/app/bin/h com.example.app\n"                                         },
        {"[Socket]\nExecStopPre=/app/bin/a\n",
         "[Socket]\nExecStopPre=" MF_MFRUN_PATH " --command=/app/bin/a com.example.app\n"                                                                                                   },

        /* a failure may be ignored, and a key can be cleared */
        {"[Service]\nExecStartPre=-/app/bin/a b\n",
         "[Service]\nExecStartPre=-" MF_MFRUN_PATH " --command=/app/bin/a com.example.app b\n"                                                                                              },
        {"[Service]\nExecStart=-app\n",                                                                                          "[Service]\nExecStart=-" MF_MFRUN_PATH " com.example.app\n"},
        {"[Service]\nExecStart=\nExecStart=app\n",
         "[Service]\nExecStart=\nExecStart=" MF_MFRUN_PATH " com.example.app\n"                                                                                                             },

        /* semicolons that do not separate commands */
        {"[Service]\nExecStart=app a \\; b\n",
         "[Service]\nExecStart=" MF_MFRUN_PATH " com.example.app a \\; b\n"                                                                                                                 },
        {"[Service]\nExecStart=app --x=a;b c;\n",
         "[Service]\nExecStart=" MF_MFRUN_PATH " com.example.app --x=a;b c;\n"                                                                                                              },

        /* what is not a command stays, like comments and groups we do not know */
        {"# a\n; b \\\n[X-Foo]\nBar=1\n[Service]\nExecPaths=/app\nExecSearchPath=/app/bin\nNoExecPaths=/tmp\nEnvironment=A=B\n",
         "# a\n; b \\\n[X-Foo]\nBar=1\n[Service]\nExecPaths=/app\nExecSearchPath=/app/bin\nNoExecPaths=/tmp\nEnvironment=A=B\n"                                                             },

        /* Alias and Also can name units of the component */
        {"[Install]\nAlias=com.example.app-two.service com.example.app.three.service\nAlso=com.example.app.socket\n",
         "[Install]\nAlias=com.example.app-two.service com.example.app.three.service\nAlso=com.example.app.socket\n"                                                                        },
        {"[Install]\nAlias=\n",                                                                                                  "[Install]\nAlias=\n"                                      },
    };

    for (gsize i = 0; i < G_N_ELEMENTS(units); i++) {
        g_autoptr(GBytes) content = g_bytes_new_static(units[i].content, strlen(units[i].content));
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) rendered = NULL;
        g_autofree char *text = NULL;
        gsize size;
        const char *data;

        g_test_message("%s", units[i].content);
        rendered = mft_render("systemd-unit-system", UNIT_PATH, content, &error);
        g_assert_no_error(error);
        data = g_bytes_get_data(rendered, &size);
        text = g_strndup(data, size);
        g_assert_cmpstr(text, ==, units[i].expected);
    }
}

static void
test_units_refused(void)
{
#define UNIT(text) {text, sizeof(text) - 1}

    static const struct {
        const char *content;
        gsize size;
    } units[] = {
        /* several commands in one key, of which only the first one would run in the image */
        UNIT("[Service]\nExecStart=/app/bin/app ; /bin/evil\n"),
        UNIT("[Service]\nExecStart=/app/bin/app\t;\t/bin/evil\n"),
        UNIT("[Service]\nExecStart=/app/bin/app  ;  /bin/evil\n"),
        UNIT("[Service]\nExecStart=/app/bin/app a ;\n"),
        UNIT("[Service]\nExecStart=/app/bin/app \"a\" ; /bin/evil\n"),
        UNIT("[Service]\nExecStart=-/app/bin/app ; /bin/evil\n"),
        UNIT("[Service]\nExecStart=; /bin/evil\n"),
        UNIT("[Service]\nExecStart=;\n"),

        /* prefixes that change how the command runs */
        UNIT("[Service]\nExecStart=+/app/bin/app\n"),
        UNIT("[Service]\nExecStart=!/app/bin/app\n"),
        UNIT("[Service]\nExecStart=!!/app/bin/app\n"),
        UNIT("[Service]\nExecStart=:/app/bin/app\n"),
        UNIT("[Service]\nExecStart=@/app/bin/app argv0\n"),
        UNIT("[Service]\nExecStart=|/app/bin/app\n"),
        UNIT("[Service]\nExecStart=-+/app/bin/app\n"),
        UNIT("[Service]\nExecStart=--/app/bin/app\n"),
        UNIT("[Service]\nExecStart=- /app/bin/app\n"),

        /* commands that are not programs of the image */
        UNIT("[Service]\nExecStart=/bin/sh -c evil\n"),
        UNIT("[Service]\nExecStart= /bin/evil\n"),
        UNIT("[Service]\nExecStart=/usr/bin/app\n"),
        UNIT("[Service]\nExecStart=%h/evil\n"),
        UNIT("[Service]\nExecStart=$EVIL\n"),
        UNIT("[Service]\nExecStart=\"/app/bin/app\"\n"),
        UNIT("[Service]\nExecStart=/app/bin/a\tb\n"),

        /* keys that may run a command in a version of systemd we do not know */
        UNIT("[Service]\nExecFuture=/bin/evil\n"),
        UNIT("[Service]\nExecStartLater=/bin/evil\n"),

        /* lines that we and systemd would see differently */
        UNIT("[Service]\nExecStart=/app/bin/app \\\n/bin/evil\n"),
        UNIT("[Service]\nDescription=x \\\nExecStart=/bin/evil\n"),
        /* each line is fine on its own, but joined the semicolon separates two commands */
        UNIT("[Service]\nExecStart=/app/bin/app \\\nEnvironment=a ; /bin/evil\n"),
        UNIT("[Service]\nDescription=x\rExecStartPre=+/bin/sh -c evil\n"),
        UNIT("[Service]\nDescription=x\n\0ExecStartPre=+/bin/sh -c evil\n"),
        UNIT("\xef\xbb\xbf[Service]\nExecStart=/bin/evil\n"),
        UNIT("[Service]\n ExecStart=/bin/evil\n"),
        UNIT("[Service]\n\tExecStart=/bin/evil\n"),
        UNIT("[Service]\nExecStart =/bin/evil\n"),
        UNIT("[Service]\nExecStart = /app/bin/app\n"),
        UNIT("[Service]\nno key\n"),
        UNIT("ExecStart=/bin/evil\n"),
        UNIT("[Service\nExecStart=/bin/evil\n"),
        UNIT("[]\nExecStart=/bin/evil\n"),
        UNIT("[Service]x\nExecStart=/bin/evil\n"),
        UNIT("[Service]\nExec[de]=/bin/evil\n"),

        /* units of others */
        UNIT("[Install]\nAlias=sshd.service\n"),
        UNIT("[Install]\nAlias=com.example.app-two.service display-manager.service\n"),
        UNIT("[Install]\nAlso=other.socket\n"),
    };

#undef UNIT

    for (gsize i = 0; i < G_N_ELEMENTS(units); i++) {
        g_autoptr(GBytes) content = g_bytes_new_static(units[i].content, units[i].size);
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) rendered = NULL;

        g_test_message("%s", units[i].content);
        rendered = mft_render("systemd-unit-system", UNIT_PATH, content, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(rendered);
    }
}

#define MIME_PATH  "/app/share/mime/packages/com.example.app.xml"
#define MIME_XML   "<?xml version='1.0' encoding='UTF-8'?>\n"
#define MIME_NS    "http://www.freedesktop.org/standards/shared-mime-info"
#define MIME_OPEN  "<mime-info xmlns='" MIME_NS "'>"
#define MIME_CLOSE "</mime-info>"
#define MIME_TYPE  "<mime-type type='application/x-app'>"
#define MIME_END   "</mime-type>"

static void
test_mime_packages(void)
{
    /* the tables use single quotes to avoid escapes, and we convert the expected text to double quotes */
    static const struct {
        const char *content;
        const char *expected;
    } packages[] = {
        /* what the test application ships */
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

        /* what is allowed in a type */
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

        /* what another parser could read differently does not reach it */
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

    for (gsize i = 0; i < G_N_ELEMENTS(packages); i++) {
        g_autoptr(GBytes) content = g_bytes_new_static(packages[i].content, strlen(packages[i].content));
        g_autofree char *expected = g_strdelimit(g_strdup(packages[i].expected), "'", '"');
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) rendered = NULL;
        g_autofree char *text = NULL;
        gsize size;
        const char *data;

        g_test_message("%s", packages[i].content);
        rendered = mft_render("mime-package", MIME_PATH, content, &error);
        g_assert_no_error(error);
        data = g_bytes_get_data(rendered, &size);
        text = g_strndup(data, size);
        g_assert_cmpstr(text, ==, expected);
    }
}

static void
test_mime_packages_refused(void)
{
    static const char *const packages[] = {
        /* changing what the host knows */
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

        /* the shape of the document */
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

        /* claiming files of other types */
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

        /* no plain text */
        MIME_OPEN MIME_TYPE "<comment>\r</comment>" MIME_END MIME_CLOSE,
        MIME_OPEN MIME_TYPE "<comment>\xff</comment>" MIME_END MIME_CLOSE,
    };

    for (gsize i = 0; i < G_N_ELEMENTS(packages); i++) {
        g_autoptr(GBytes) content = g_bytes_new_static(packages[i], strlen(packages[i]));
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) rendered = NULL;

        g_test_message("%s", packages[i]);
        rendered = mft_render("mime-package", MIME_PATH, content, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(rendered);
    }
}

#define RULE_PATH "/app/lib/udev/rules.d/60-com.example.app.rules"

static void
test_udev_rules(void)
{
    static const char *const rules[] = {
        /* what the test application ships, and what instruments usually need */
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

    for (gsize i = 0; i < G_N_ELEMENTS(rules); i++) {
        g_autoptr(GBytes) content = g_bytes_new_static(rules[i], strlen(rules[i]));
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) rendered = NULL;

        g_test_message("%s", rules[i]);
        rendered = mft_render("udev-rule", RULE_PATH, content, &error);
        g_assert_no_error(error);
        g_assert_true(g_bytes_equal(rendered, content));
    }
}

static void
test_udev_rules_refused(void)
{
#define RULE(text) {text, sizeof(text) - 1}

    static const struct {
        const char *content;
        gsize size;
    } rules[] = {
        /* running programs */
        RULE("RUN+=\"/bin/evil\"\n"),
        RULE("RUN=\"/bin/evil\"\n"),
        RULE("RUN:=\"/bin/evil\"\n"),
        RULE("RUN{program}+=\"/bin/evil\"\n"),
        RULE("RUN{builtin}+=\"kmod load evil\"\n"),
        RULE("PROGRAM==\"/bin/evil\"\n"),
        RULE("PROGRAM=\"/bin/evil\"\n"),
        RULE("PROGRAM!=\"/bin/evil\"\n"),
        RULE("IMPORT{program}=\"/bin/evil\"\n"),
        RULE("IMPORT{program}==\"/bin/evil\"\n"),
        RULE("KERNEL==\"a\", MODE=\"0600\", RUN+=\"/bin/evil\"\n"),
        RULE("KERNEL==\"a\" RUN+=\"/bin/evil\"\n"),
        RULE("KERNEL==\"a\",RUN+=\"/bin/evil\"\n"),
        RULE("KERNEL==\"a\"RUN+=\"/bin/evil\"\n"),
        RULE("\tRUN+=\"/bin/evil\"\n"),

        /* changes outside of the matched device */
        RULE("SYSCTL{kernel/core_pattern}=\"|/bin/evil\"\n"),
        RULE("ATTR{[block/sda]size}=\"1\"\n"),
        RULE("ATTR{$env{X}}=\"1\"\n"),
        RULE("ATTR{%k}=\"1\"\n"),

        /* units of others */
        RULE("ENV{SYSTEMD_WANTS}+=\"sshd.service\"\n"),
        RULE("ENV{SYSTEMD_WANTS}=\"com.example.app.service other.service\"\n"),
        RULE("ENV{SYSTEMD_USER_WANTS}+=\"other.service\"\n"),
        RULE("ENV{SYSTEMD_WANTS}+=\"$env{X}\"\n"),
        RULE("ENV{SYSTEMD_WANTS}+=\"%E{X}.service\"\n"),
        RULE("ENV{SYSTEMD_WANTS}+=\"other@com.example.app.service\"\n"),
        RULE("ENV{SYSTEMD_WANTS}+=\"com.example.app.mount\"\n"),

        /* values that could end somewhere else for udev */
        RULE("KERNEL==\"a\\\", RUN+=\"/bin/evil\"\n"),
        RULE("KERNEL==\"a\\\\\", RUN+=\"/bin/evil\"\n"),
        RULE("KERNEL==e\"a\\\\\", MODE=\"0666\"\n"),
        RULE("KERNEL==i\"a\"\n"),
        RULE("KERNEL==ei\"a\"\n"),
        RULE("KERNEL==\"a\\\"\n"),
        RULE("MODE=0666\n"),
        RULE("MODE=\"0666\n"),

        /* lines that we and udev would see differently */
        RULE("KERNEL==\"a\", \\\nRUN+=\"/bin/evil\"\n"),
        RULE("KERNEL==\"a\", \\\n# comment\nMODE=\"0600\"\n"),
        RULE("KERNEL==\"a\"\rRUN+=\"/bin/evil\"\n"),
        RULE("KERNEL==\"a\"\n\0RUN+=\"/bin/evil\"\n"),
        RULE("\xef\xbb\xbfKERNEL==\"a\"\n"),

        /* what is not a plain rule */
        RULE("KERNEL == \"a\"\n"),
        RULE("KERNEL= =\"a\"\n"),
        RULE("KERNEL\n"),
        RULE("kernel==\"a\"\n"),
        RULE("ATTR{a==\"b\"\n"),
        RULE("ATTR{a b}==\"c\"\n"),
        RULE("ATTR{a\"b}==\"c\"\n"),
        RULE("ENV{\"SYSTEMD_WANTS\"}+=\"sshd.service\"\n"),
    };

#undef RULE

    for (gsize i = 0; i < G_N_ELEMENTS(rules); i++) {
        g_autoptr(GBytes) content = g_bytes_new_static(rules[i].content, rules[i].size);
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) rendered = NULL;

        g_test_message("%s", rules[i].content);
        rendered = mft_render("udev-rule", RULE_PATH, content, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(rendered);
    }
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/integration/kind-strings", test_kind_strings);
    g_test_add_func("/integration/privileged", test_privileged);
    g_test_add_func("/integration/section", test_section);
    g_test_add_func("/integration/no-section", test_no_section);
    g_test_add_func("/integration/malformed-section", test_malformed_section);
    g_test_add_func("/integration/check-file-paths", test_check_file_paths);
    g_test_add_func("/integration/check-aliases", test_check_aliases);
    g_test_add_func("/integration/render-aliases", test_render_aliases);
    g_test_add_func("/integration/check-section", test_check_section);
    g_test_add_func("/integration/check-component", test_check_component);
    g_test_add_func("/integration/check-messages", test_check_messages);
    g_test_add_func("/integration/manifest-messages", test_manifest_messages);
    g_test_add_func("/integration/desktop-entries", test_desktop_entries);
    g_test_add_func("/integration/desktop-entries-refused", test_desktop_entries_refused);
    g_test_add_func("/integration/text-size-limit", test_text_size_limit);
    g_test_add_func("/integration/units", test_units);
    g_test_add_func("/integration/units-refused", test_units_refused);
    g_test_add_func("/integration/mime-packages", test_mime_packages);
    g_test_add_func("/integration/mime-packages-refused", test_mime_packages_refused);
    g_test_add_func("/integration/udev-rules", test_udev_rules);
    g_test_add_func("/integration/udev-rules-refused", test_udev_rules_refused);

    return g_test_run();
}
