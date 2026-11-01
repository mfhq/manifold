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
 * test_app:
 * @integration: (nullable): the integration section as JSON
 *
 * Returns: the manifest of an application
 */
static MftTestManifest
test_app(const char *integration)
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
 * test_manifest_new:
 * @integration: (nullable): the integration section as JSON
 *
 * Returns: (transfer full) (nullable): the parsed manifest of an application
 */
static MfManifest *
test_manifest_new(const char *integration, GError **error)
{
    MftTestManifest m = test_app(integration);
    g_autofree char *json = mft_manifest_json(&m);

    return mf_manifest_new_from_data(json, -1, error);
}

/**
 * test_passes_check:
 * @m: a manifest that can be parsed
 *
 * Returns: whether mf_manifest_check_integration() accepts the manifest
 */
static gboolean
test_passes_check(const MftTestManifest *m)
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
        {"icons",               "/app/share/icons/hicolor/48x48/apps/com.example.app.jxl",             FALSE},
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
        MftTestManifest m = test_app(section);

        g_test_message("%s %s", files[i].kind, files[i].path);
        g_assert_cmpint(test_passes_check(&m), ==, files[i].valid);
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

        /* a tool a host may not have yet */
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
        MftTestManifest m = test_app(section);

        g_test_message("%s %s", aliases[i].name, aliases[i].command);
        g_assert_cmpint(test_passes_check(&m), ==, aliases[i].valid);
    }
}

static void
test_check_section(void)
{
    MftTestManifest m = test_app("{\"from-the-future\": []}");

    /* a kind we do not know must not be dropped silently */
    g_assert_false(test_passes_check(&m));

    m.integration =
        "{\"alias\": [{\"name\": \"app\", \"command\": \"/app/bin/a\"},"
        " {\"name\": \"app\", \"command\": \"/app/bin/b\"}]}";
    g_assert_false(test_passes_check(&m));

    m.integration = "{\"mime-package\": [{\"path\": \"/app/share/mime/packages/com.example.app.xml\", " HASHES
                    "},"
                    " {\"path\": \"/app/share/mime/packages/com.example.app.xml\", " HASHES "}]}";
    g_assert_false(test_passes_check(&m));

    m.integration = NULL;
    g_assert_true(test_passes_check(&m));
    m.integration = "{}";
    g_assert_true(test_passes_check(&m));
}

static void
test_check_component(void)
{
    static const char *const refused_ids[] = {"ssh", "com.example", "com..example", "com.example."};
    static const char *const refused_kinds[] = {"kit", "platform", "plugin"};
    MftTestManifest m = test_app("{}");

    for (gsize i = 0; i < G_N_ELEMENTS(refused_ids); i++) {
        m.component_id = refused_ids[i];
        g_test_message("%s", refused_ids[i]);
        g_assert_false(test_passes_check(&m));
    }

    m = test_app("{}");
    for (gsize i = 0; i < G_N_ELEMENTS(refused_kinds); i++) {
        m.kind = refused_kinds[i];
        g_test_message("%s", refused_kinds[i]);
        g_assert_false(test_passes_check(&m));
    }

    m = test_app("{}");
    m.sdk = TRUE;
    g_assert_false(test_passes_check(&m));

    /* what can not use the section passes if it does not */
    m.integration = NULL;
    g_assert_true(test_passes_check(&m));
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
        g_autoptr(MfManifest) manifest = test_manifest_new(sections[i], &error);

        g_assert_no_error(error);
        g_assert_false(mf_manifest_check_integration(manifest, &error));
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_test_message("%s", error->message);
        g_assert_null(strchr(error->message, '\033'));
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
    g_test_add_func("/integration/check-file-paths", test_check_file_paths);
    g_test_add_func("/integration/check-aliases", test_check_aliases);
    g_test_add_func("/integration/check-section", test_check_section);
    g_test_add_func("/integration/check-component", test_check_component);
    g_test_add_func("/integration/check-messages", test_check_messages);

    return g_test_run();
}
