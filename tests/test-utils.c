/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <manifold.h>

#include "mf-component-private.h"
#include "mf-utils-private.h"

/**
 * test_version_is_valid:
 *
 * A version is used as the name of a directory in the store.
 */
static void
test_version_is_valid(void)
{
    g_assert_true(mf_version_is_valid("0.1.0"));
    g_assert_true(mf_version_is_valid("13.7a"));
    g_assert_true(mf_version_is_valid("2.0+dfsg~rc1_3"));
    g_assert_false(mf_version_is_valid("1:2.0")); /* no epochs */
    /* a dash is allowed; the store splits off the revision at the last dash */
    g_assert_true(mf_version_is_valid("1.0-2"));
    /* "current" is reserved, as it is the name of the link to the current version in the store */
    g_assert_false(mf_version_is_valid("current"));
    g_assert_false(mf_version_is_valid(NULL));
    g_assert_false(mf_version_is_valid(""));
    g_assert_false(mf_version_is_valid("1/2"));
}

/**
 * test_hex_hash_is_valid:
 *
 * A hash is 64 lowercase hex digits.
 */
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

/**
 * test_relative_path_is_valid:
 *
 * A path in a repository stays below the directory of the repository.
 */
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
    for (size_t i = 0; i < G_N_ELEMENTS(invalid); i++)
        g_assert_false(mf_relative_path_is_valid(invalid[i]));

    /* a path may be longer than a name, up to 1024 bytes */
    for (guint i = 0; i < 204; i++)
        g_string_append(longest, "abcd/");
    g_string_append(longest, "abcd");
    g_assert_true(mf_relative_path_is_valid(longest->str));
    too_long = g_strconcat(longest->str, "e", NULL);
    g_assert_false(mf_relative_path_is_valid(too_long));
}

/**
 * test_component_id_is_valid:
 *
 * A component ID is used as the name of a directory in the store.
 */
static void
test_component_id_is_valid(void)
{
    g_autofree char *too_long = g_strnfill(256, 'a');

    g_assert_true(mf_component_id_is_valid("com.example.app"));
    g_assert_true(mf_component_id_is_valid("net.debian.core13.7d225c1fe79b1c5e"));
    g_assert_true(mf_component_id_is_valid("a_b-c"));
    g_assert_false(mf_component_id_is_valid(NULL));
    g_assert_false(mf_component_id_is_valid(""));
    g_assert_false(mf_component_id_is_valid("../etc"));
    g_assert_false(mf_component_id_is_valid(too_long));
}

/**
 * test_program_name_is_valid:
 *
 * A program name is a plain file name. This test also covers the rules that all kinds of names share.
 */
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
    /* no name starts with a dot or a dash, or has a blank */
    g_assert_false(mf_program_name_is_valid(".hidden"));
    g_assert_false(mf_program_name_is_valid("-dash"));
    g_assert_false(mf_program_name_is_valid("a/b"));
    g_assert_false(mf_program_name_is_valid("a b"));
    g_assert_false(mf_program_name_is_valid("a\nb"));
    g_assert_false(mf_program_name_is_valid("a'b"));
    g_assert_false(mf_program_name_is_valid("a;b"));
    g_assert_false(mf_program_name_is_valid("$(id)"));
    g_assert_false(mf_program_name_is_valid(too_long));
}

/**
 * test_signing_key_name_is_valid:
 *
 * The name of a signing key can not be mistaken for a path, a public key or a fingerprint.
 */
static void
test_signing_key_name_is_valid(void)
{
    g_assert_true(mf_signing_key_name_is_valid("default"));
    g_assert_true(mf_signing_key_name_is_valid("release-2026.1_a"));
    g_assert_true(mf_signing_key_name_is_valid("1"));
    g_assert_false(mf_signing_key_name_is_valid(NULL));
    g_assert_false(mf_signing_key_name_is_valid(""));
    g_assert_false(mf_signing_key_name_is_valid("_key"));
    g_assert_false(mf_signing_key_name_is_valid("./key"));
    g_assert_false(mf_signing_key_name_is_valid("a/b"));
    g_assert_false(mf_signing_key_name_is_valid("SHA256:abc"));
    g_assert_false(mf_signing_key_name_is_valid("key.pub"));
}

/**
 * test_text_is_valid:
 *
 * A text file that is installed on the host must be read the same way by every program.
 */
static void
test_text_is_valid(void)
{
    const size_t limit = 1024 * 1024;
    g_autofree char *large = g_strnfill(limit + 1, 'x');
    g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
    g_autoptr(GBytes) text = g_bytes_new_static("Name=Äpp\n", 10);
    g_autoptr(GBytes) largest = g_bytes_new_static(large, limit);
    g_autoptr(GBytes) too_large = g_bytes_new_static(large, limit + 1);
    g_autoptr(GBytes) with_nul = g_bytes_new_static("a\n\0b\n", 5);
    g_autoptr(GBytes) with_cr = g_bytes_new_static("a\rb\n", 4);
    g_autoptr(GBytes) not_utf8 = g_bytes_new_static("a\xff\n", 3);

    g_assert_true(mf_text_is_valid(empty));
    g_assert_true(mf_text_is_valid(text));
    g_assert_true(mf_text_is_valid(largest));
    g_assert_false(mf_text_is_valid(too_large));
    g_assert_false(mf_text_is_valid(with_nul));
    g_assert_false(mf_text_is_valid(with_cr));
    g_assert_false(mf_text_is_valid(not_utf8));
}

/**
 * test_display_text_is_valid:
 *
 * A text that we print on a terminal is one line of printable characters.
 */
static void
test_display_text_is_valid(void)
{
    g_autofree char *longest = g_strnfill(512, 'a');
    g_autofree char *too_long = g_strnfill(513, 'a');

    g_assert_true(mf_display_text_is_valid("Example applications"));
    g_assert_true(mf_display_text_is_valid("Äpps für alle"));
    g_assert_true(mf_display_text_is_valid(longest));
    g_assert_false(mf_display_text_is_valid(""));
    g_assert_false(mf_display_text_is_valid(too_long));
    g_assert_false(mf_display_text_is_valid("two\nlines"));
    g_assert_false(mf_display_text_is_valid("a\ttab"));
    g_assert_false(mf_display_text_is_valid("\033[2J"));
    g_assert_false(mf_display_text_is_valid("not \xff UTF-8"));
    /* U+202E changes the text direction, and can make a text read differently */
    g_assert_false(
        mf_display_text_is_valid("a\xe2\x80\xae"
                                 "b"));
}

/**
 * test_component_kind_can_build_on:
 *
 * Test the layers that a component of a kind can build on.
 */
static void
test_component_kind_can_build_on(void)
{
    const MfComponentKind core = MF_COMPONENT_KIND_CORE;
    const MfComponentKind platform = MF_COMPONENT_KIND_PLATFORM;
    const MfComponentKind kit = MF_COMPONENT_KIND_KIT;
    const MfComponentKind app = MF_COMPONENT_KIND_APP;
    const MfComponentKind plugin = MF_COMPONENT_KIND_PLUGIN;

    /* a component builds on a lower layer */
    g_assert_true(mf_component_kind_can_build_on(platform, FALSE, core, FALSE));
    g_assert_true(mf_component_kind_can_build_on(kit, FALSE, core, FALSE));
    g_assert_true(mf_component_kind_can_build_on(kit, FALSE, platform, FALSE));
    g_assert_true(mf_component_kind_can_build_on(app, FALSE, core, FALSE));
    g_assert_true(mf_component_kind_can_build_on(app, FALSE, platform, FALSE));
    g_assert_true(mf_component_kind_can_build_on(app, FALSE, kit, FALSE));
    g_assert_true(mf_component_kind_can_build_on(plugin, FALSE, app, FALSE));
    g_assert_false(mf_component_kind_can_build_on(platform, FALSE, kit, FALSE));
    g_assert_false(mf_component_kind_can_build_on(kit, FALSE, app, FALSE));
    g_assert_false(mf_component_kind_can_build_on(plugin, FALSE, kit, FALSE));
    g_assert_false(mf_component_kind_can_build_on(core, FALSE, platform, FALSE));

    /* only an SDK builds on a component of its own kind, which is not an SDK itself */
    g_assert_false(mf_component_kind_can_build_on(core, FALSE, core, FALSE));
    g_assert_false(mf_component_kind_can_build_on(kit, FALSE, kit, FALSE));
    g_assert_true(mf_component_kind_can_build_on(core, TRUE, core, FALSE));
    g_assert_true(mf_component_kind_can_build_on(kit, TRUE, kit, FALSE));
    g_assert_false(mf_component_kind_can_build_on(kit, TRUE, kit, TRUE));

    /* the SDK of a runtime only extends its own layer */
    g_assert_false(mf_component_kind_can_build_on(kit, TRUE, core, FALSE));
    g_assert_false(mf_component_kind_can_build_on(platform, TRUE, core, FALSE));

    /* an application and its SDK can build on an SDK, like a development tool does */
    g_assert_true(mf_component_kind_can_build_on(app, FALSE, core, TRUE));
    g_assert_true(mf_component_kind_can_build_on(app, TRUE, kit, TRUE));

    g_assert_false(mf_component_kind_can_build_on(MF_COMPONENT_KIND_UNKNOWN, FALSE, core, FALSE));
    g_assert_false(mf_component_kind_can_build_on(app, FALSE, MF_COMPONENT_KIND_UNKNOWN, FALSE));
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

/**
 * test_action_strings:
 *
 * Test the names of the actions.
 */
static void
test_action_strings(void)
{
    for (MfAction action = MF_ACTION_INSTALL; action < MF_ACTION_LAST; action++)
        g_assert_cmpint(mf_action_from_string(mf_action_to_string(action)), ==, action);
    g_assert_cmpint(mf_action_from_string("bogus"), ==, MF_ACTION_UNKNOWN);
}

/**
 * test_ptr_array_join_strings:
 *
 * Join the strings of an array.
 */
static void
test_ptr_array_join_strings(void)
{
    g_autoptr(GPtrArray) strings = g_ptr_array_new();
    g_autofree char *none = mf_ptr_array_join_strings(strings, ", ");
    g_autofree char *one = NULL;
    g_autofree char *two = NULL;

    g_assert_cmpstr(none, ==, "");
    g_ptr_array_add(strings, "a");
    one = mf_ptr_array_join_strings(strings, ", ");
    g_assert_cmpstr(one, ==, "a");
    g_ptr_array_add(strings, "b");
    two = mf_ptr_array_join_strings(strings, ", ");
    g_assert_cmpstr(two, ==, "a, b");
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/utils/version-is-valid", test_version_is_valid);
    g_test_add_func("/utils/hex-hash-is-valid", test_hex_hash_is_valid);
    g_test_add_func("/utils/relative-path-is-valid", test_relative_path_is_valid);
    g_test_add_func("/utils/component-id-is-valid", test_component_id_is_valid);
    g_test_add_func("/utils/program-name-is-valid", test_program_name_is_valid);
    g_test_add_func("/utils/text-is-valid", test_text_is_valid);
    g_test_add_func("/utils/display-text-is-valid", test_display_text_is_valid);
    g_test_add_func("/utils/component-kind-can-build-on", test_component_kind_can_build_on);
    g_test_add_func("/utils/architecture-is-valid", test_architecture_is_valid);
    g_test_add_func("/utils/signing-key-name-is-valid", test_signing_key_name_is_valid);
    g_test_add_func("/utils/component-kind-strings", test_component_kind_strings);
    g_test_add_func("/utils/action-strings", test_action_strings);
    g_test_add_func("/utils/ptr-array-join-strings", test_ptr_array_join_strings);

    return g_test_run();
}
