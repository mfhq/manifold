/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <manifold.h>

#include "mf-utils-private.h"

static void
test_version_is_valid(void)
{
    g_assert_true(mf_version_is_valid("0.1.0"));
    g_assert_true(mf_version_is_valid("13.7a"));
    g_assert_true(mf_version_is_valid("2.0+dfsg~rc1_3"));
    g_assert_false(mf_version_is_valid("1:2.0")); /* no epochs */
    /* a dash is allowed; the store splits off the revision at the last dash */
    g_assert_true(mf_version_is_valid("1.0-2"));
    /* reserved: the store's "current" link names the active version */
    g_assert_false(mf_version_is_valid("current"));
    g_assert_false(mf_version_is_valid(NULL));
    g_assert_false(mf_version_is_valid(""));
    g_assert_false(mf_version_is_valid(".1"));
    g_assert_false(mf_version_is_valid("-1"));
    g_assert_false(mf_version_is_valid("1/2"));
    g_assert_false(mf_version_is_valid("1 2"));
}

static void
test_version_compare(void)
{
    g_assert_cmpint(mf_version_compare("0.2.0", "0.10.0"), <, 0);
    g_assert_cmpint(mf_version_compare("13.7a", "13.7"), >, 0);
    g_assert_cmpint(mf_version_compare("1.0", "1.0"), ==, 0);
}

static void
test_hex_hash_is_valid(void)
{
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "x", -1);
    g_autofree char *upper = g_ascii_strup(hash, -1);
    g_autofree char *shorter = g_strndup(hash, 63);
    g_autofree char *longer = g_strconcat(hash, "0", NULL);

    g_assert_true(mf_hex_hash_is_valid(hash));
    g_assert_false(mf_hex_hash_is_valid(NULL));
    g_assert_false(mf_hex_hash_is_valid(upper));
    g_assert_false(mf_hex_hash_is_valid(shorter));
    g_assert_false(mf_hex_hash_is_valid(longer));
}

static void
test_relative_path_is_valid(void)
{
    g_autoptr(GString) longest = g_string_new(NULL);
    g_autofree char *too_long = NULL;
    static const char *const invalid[] = {
        "",
        "/pool/a.json.zst",
        "pool//a.json.zst",
        "pool/",
        "../pool/a.json.zst",
        "pool/../a",
        "pool/./a",
        "pool/.hidden",
        "pool\\a",
        "pool/a\nb",
    };

    g_assert_true(mf_relative_path_is_valid("pool/org/example/App/0dbf23ee.json.zst"));
    g_assert_true(mf_relative_path_is_valid("main/x86_64/media/org/example/App/3f0c/icons/64x64@2/App.jxl"));
    g_assert_true(mf_relative_path_is_valid("index.json"));
    g_assert_false(mf_relative_path_is_valid(NULL));
    for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++)
        g_assert_false(mf_relative_path_is_valid(invalid[i]));

    /* a path may be longer than a name, up to 1024 bytes */
    for (guint i = 0; i < 204; i++)
        g_string_append(longest, "abcd/");
    g_string_append(longest, "abcd");
    g_assert_true(mf_relative_path_is_valid(longest->str));
    too_long = g_strconcat(longest->str, "e", NULL);
    g_assert_false(mf_relative_path_is_valid(too_long));
}

static void
test_component_id_is_valid(void)
{
    g_autofree char *too_long = g_strnfill(256, 'a');

    g_assert_true(mf_component_id_is_valid("com.example.app"));
    g_assert_true(mf_component_id_is_valid("net.debian.core13.7d225c1fe79b1c5e"));
    g_assert_true(mf_component_id_is_valid("a_b-c"));
    g_assert_false(mf_component_id_is_valid(NULL));
    g_assert_false(mf_component_id_is_valid(""));
    g_assert_false(mf_component_id_is_valid(".hidden"));
    g_assert_false(mf_component_id_is_valid("-dash"));
    g_assert_false(mf_component_id_is_valid("../etc"));
    g_assert_false(mf_component_id_is_valid("with space"));
    g_assert_false(mf_component_id_is_valid(too_long));
}

static void
test_program_name_is_valid(void)
{
    g_autofree char *longest = g_strnfill(255, 'a');
    g_autofree char *too_long = g_strnfill(256, 'a');

    g_assert_true(mf_program_name_is_valid("mftestapp"));
    g_assert_true(mf_program_name_is_valid("g++"));
    g_assert_true(mf_program_name_is_valid("foo_bar-1.2"));
    g_assert_true(mf_program_name_is_valid(longest));
    g_assert_false(mf_program_name_is_valid(NULL));
    g_assert_false(mf_program_name_is_valid(""));
    g_assert_false(mf_program_name_is_valid(".hidden"));
    g_assert_false(mf_program_name_is_valid("-dash"));
    g_assert_false(mf_program_name_is_valid("a/b"));
    g_assert_false(mf_program_name_is_valid("a b"));
    g_assert_false(mf_program_name_is_valid("a\nb"));
    g_assert_false(mf_program_name_is_valid("a'b"));
    g_assert_false(mf_program_name_is_valid("$(id)"));
    g_assert_false(mf_program_name_is_valid(too_long));
}

static void
test_signing_key_name_is_valid(void)
{
    g_assert_true(mf_signing_key_name_is_valid("default"));
    g_assert_true(mf_signing_key_name_is_valid("release-2026.1_a"));
    g_assert_true(mf_signing_key_name_is_valid("1"));
    g_assert_false(mf_signing_key_name_is_valid(NULL));
    g_assert_false(mf_signing_key_name_is_valid(""));
    g_assert_false(mf_signing_key_name_is_valid("_key"));
    g_assert_false(mf_signing_key_name_is_valid(".key"));
    g_assert_false(mf_signing_key_name_is_valid("-key"));
    g_assert_false(mf_signing_key_name_is_valid("./key"));
    g_assert_false(mf_signing_key_name_is_valid("a/b"));
    g_assert_false(mf_signing_key_name_is_valid("a b"));
    g_assert_false(mf_signing_key_name_is_valid("SHA256:abc"));
    g_assert_false(mf_signing_key_name_is_valid("key.pub"));
}

static void
test_architecture_is_valid(void)
{
    g_autofree char *too_long = g_strnfill(65, 'a');

    g_assert_true(mf_architecture_is_valid(mf_get_host_architecture()));
    g_assert_true(mf_architecture_is_valid("x86_64"));
    g_assert_true(mf_architecture_is_valid("aarch64"));
    g_assert_true(mf_architecture_is_valid("ppc64le"));
    g_assert_false(mf_architecture_is_valid(NULL));
    g_assert_false(mf_architecture_is_valid(""));
    g_assert_false(mf_architecture_is_valid("a b"));
    g_assert_false(mf_architecture_is_valid("a/b"));
    g_assert_false(mf_architecture_is_valid("\033[2J"));
    g_assert_false(mf_architecture_is_valid(too_long));
}

static void
test_component_kind_strings(void)
{
    for (MfComponentKind kind = MF_COMPONENT_KIND_CORE; kind < MF_COMPONENT_KIND_LAST; kind++)
        g_assert_cmpint(mf_component_kind_from_string(mf_component_kind_to_string(kind)), ==, kind);
    g_assert_cmpint(mf_component_kind_from_string("bogus"), ==, MF_COMPONENT_KIND_UNKNOWN);
    g_assert_cmpint(mf_component_kind_from_string(NULL), ==, MF_COMPONENT_KIND_UNKNOWN);
    g_assert_cmpstr(mf_component_kind_to_string(MF_COMPONENT_KIND_APP), ==, "app");
}

static void
test_action_strings(void)
{
    for (MfAction action = MF_ACTION_INSTALL; action < MF_ACTION_LAST; action++)
        g_assert_cmpint(mf_action_from_string(mf_action_to_string(action)), ==, action);
    g_assert_cmpint(mf_action_from_string("bogus"), ==, MF_ACTION_UNKNOWN);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/utils/version-is-valid", test_version_is_valid);
    g_test_add_func("/utils/version-compare", test_version_compare);
    g_test_add_func("/utils/hex-hash-is-valid", test_hex_hash_is_valid);
    g_test_add_func("/utils/relative-path-is-valid", test_relative_path_is_valid);
    g_test_add_func("/utils/component-id-is-valid", test_component_id_is_valid);
    g_test_add_func("/utils/program-name-is-valid", test_program_name_is_valid);
    g_test_add_func("/utils/architecture-is-valid", test_architecture_is_valid);
    g_test_add_func("/utils/signing-key-name-is-valid", test_signing_key_name_is_valid);
    g_test_add_func("/utils/component-kind-strings", test_component_kind_strings);
    g_test_add_func("/utils/action-strings", test_action_strings);

    return g_test_run();
}
