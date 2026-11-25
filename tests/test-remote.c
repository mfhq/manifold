/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <glib/gstdio.h>
#include <manifold.h>

#include "mf-remote-private.h"
#include "mft-utils.h"

typedef struct {
    char *dir;
    char *vendor_dir;
    char *admin_dir;
} Fixture;

static void
mft_setup(Fixture *fixture, gconstpointer user_data)
{
    fixture->dir = mft_make_tmp_dir("remote");
    fixture->vendor_dir = g_build_filename(fixture->dir, "usr-share", NULL);
    fixture->admin_dir = g_build_filename(fixture->dir, "etc", NULL);
}

static void
mft_teardown(Fixture *fixture, gconstpointer user_data)
{
    mft_rm_rf(fixture->dir);
    g_free(fixture->dir);
    g_free(fixture->vendor_dir);
    g_free(fixture->admin_dir);
}

/**
 * mft_write_remote:
 * @config: (nullable): the remote.conf to write, or %NULL to mask the remote
 */
static void
mft_write_remote(const char *dir, const char *remote_id, const char *config)
{
    g_autofree char *remote_dir = g_build_filename(dir, remote_id, NULL);
    g_autofree char *path = g_build_filename(remote_dir, "remote.conf", NULL);

    mft_make_dirs(remote_dir);
    if (config != NULL)
        mft_write_file(path, config);
    else
        mft_make_link("/dev/null", path);
}

/**
 * mft_install_key:
 *
 * Copy the public key of the key at @key_path into the key directory @dir.
 */
static void
mft_install_key(const char *dir, const char *key_path)
{
    g_autofree char *public_key = g_strconcat(key_path, ".pub", NULL);
    g_autofree char *name = g_path_get_basename(public_key);
    g_autofree char *dest = g_build_filename(dir, name, NULL);
    g_autofree char *content = mft_read_file(public_key);

    mft_make_dirs(dir);
    mft_write_file(dest, content);
}

/**
 * mft_check_trusted:
 *
 * Check whether @keys accept a repository signature of the key at @key_path.
 */
static void
mft_check_trusted(MfTrustedKeys *keys, const char *key_path, gboolean trusted)
{
    g_autoptr(GBytes) data = g_bytes_new_static("index\n", 6);
    g_autoptr(GBytes) signature = mft_sign(key_path, MF_SIGNATURE_SCOPE_REPO, data);
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(mf_trusted_keys_verify(keys, MF_SIGNATURE_SCOPE_REPO, data, signature, NULL, &error),
                    ==,
                    trusted);
    g_clear_error(&error);
}

/**
 * test_config_override:
 *
 * The remote.conf of the administrator replaces the remote.conf of the vendor.
 */
static void
test_config_override(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRemote) remote = NULL;

    /* without Channels, the remote has the main channel, and the URL loses its trailing slash */
    mft_write_remote(f->vendor_dir, "example", "[Remote]\nURL=https://example.org/repo/\n");
    remote = mf_remote_new_for_dirs(f->vendor_dir, f->admin_dir, NULL, "example", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_remote_get_id(remote), ==, "example");
    g_assert_cmpstr(mf_remote_get_url(remote), ==, "https://example.org/repo");
    g_assert_cmpstrv(mf_remote_get_channels(remote), ((const char *[]){"main", NULL}));
    g_clear_object(&remote);

    mft_write_remote(f->admin_dir,
                     "example",
                     "[Remote]\nURL=https://mirror.example.org/repo\nChannels=main;beta;\n");
    remote = mf_remote_new_for_dirs(f->vendor_dir, f->admin_dir, NULL, "example", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_remote_get_url(remote), ==, "https://mirror.example.org/repo");
    g_assert_cmpstrv(mf_remote_get_channels(remote), ((const char *[]){"main", "beta", NULL}));
}

/**
 * test_trusted_keys:
 *
 * A remote trusts the keys of the vendor and of the administrator, and
 * the administrator can mask a key of the vendor.
 */
static void
test_trusted_keys(Fixture *f, gconstpointer user_data)
{
    g_autofree char *vendor_keys = g_build_filename(f->vendor_dir, "example", "keys", NULL);
    g_autofree char *admin_keys = g_build_filename(f->admin_dir, "example", "keys", NULL);
    g_autofree char *mask = g_build_filename(admin_keys, "masked.pub", NULL);
    g_autofree char *work = g_build_filename(f->dir, "work", NULL);
    g_autofree char *vendor_key = mft_make_key(work, "vendor");
    g_autofree char *masked_key = mft_make_key(work, "masked");
    g_autofree char *admin_key = mft_make_key(work, "admin");
    g_autoptr(MfRemote) remote = NULL;
    g_autoptr(MfTrustedKeys) keys = NULL;
    g_autoptr(GError) error = NULL;

    mft_write_remote(f->vendor_dir, "example", "[Remote]\nURL=https://example.org/repo\n");
    mft_install_key(vendor_keys, vendor_key);
    mft_install_key(vendor_keys, masked_key);
    mft_install_key(admin_keys, admin_key);
    mft_make_link("/dev/null", mask);

    remote = mf_remote_new_for_dirs(f->vendor_dir, f->admin_dir, NULL, "example", &error);
    g_assert_no_error(error);
    keys = mf_remote_load_trusted_keys(remote, &error);
    g_assert_no_error(error);
    mft_check_trusted(keys, vendor_key, TRUE);
    mft_check_trusted(keys, admin_key, TRUE);
    mft_check_trusted(keys, masked_key, FALSE);
}

/**
 * test_state:
 *
 * A remote reads its summary, publisher and expiry date from the cached root index.
 */
static void
test_state(Fixture *f, gconstpointer user_data)
{
    /* clang-format off */
    const char *json =
        "{"
        "  \"id\": \"example\","
        "  \"summary\": \"Example applications\","
        "  \"publisher\": {\"name\": \"Example Inc.\", \"email\": \"repo@example.org\"},"
        "  \"serial\": 1,"
        "  \"date\": \"2026-11-02T18:00:00Z\","
        "  \"valid-until\": \"2027-05-04T14:57:58Z\","
        "  \"channels\": {}"
        "}";
    /* clang-format on */
    g_autofree char *state_dir = g_build_filename(f->dir, "var-lib", NULL);
    g_autofree char *remote_state = g_build_filename(state_dir, "example", NULL);
    g_autofree char *index_path = g_build_filename(remote_state, "repo-index.json.zst", NULL);
    g_autoptr(GBytes) index = mft_zstd(json);
    g_autoptr(MfRemote) remote = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *valid_until = NULL;
    MfRemoteInfo *info;

    mft_write_remote(f->vendor_dir, "example", "[Remote]\nURL=https://example.org/repo\n");

    /* nothing is known before the first refresh */
    remote = mf_remote_new_for_dirs(f->vendor_dir, f->admin_dir, state_dir, "example", &error);
    g_assert_no_error(error);
    g_assert_null(mf_remote_get_info(remote));
    g_clear_object(&remote);

    mft_make_dirs(remote_state);
    mft_write_bytes(index_path, index);
    remote = mf_remote_new_for_dirs(f->vendor_dir, f->admin_dir, state_dir, "example", &error);
    g_assert_no_error(error);
    info = mf_remote_get_info(remote);
    g_assert_nonnull(info);
    g_assert_cmpstr(mf_remote_info_get_id(info), ==, "example");
    g_assert_cmpstr(mf_remote_info_get_url(info), ==, "https://example.org/repo");
    g_assert_cmpstr(mf_remote_info_get_summary(info), ==, "Example applications");
    g_assert_cmpstr(mf_remote_info_get_publisher_name(info), ==, "Example Inc.");
    g_assert_cmpstr(mf_remote_info_get_publisher_email(info), ==, "repo@example.org");
    /* the cached root index is not verified, so the description has no key */
    g_assert_null(mf_remote_info_get_public_key(info));
    g_assert_null(mf_remote_info_get_fingerprint(info));
    g_assert_cmpuint(g_strv_length((GStrv) mf_remote_info_get_available_channels(info)), ==, 0);
    valid_until = g_date_time_format_iso8601(mf_remote_info_get_valid_until(info));
    g_assert_cmpstr(valid_until, ==, "2027-05-04T14:57:58Z");
}

/**
 * test_list:
 *
 * Remotes are listed by their ID, from both directories, without the masked ones.
 */
static void
test_list(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) remotes = NULL;
    g_autoptr(MfRemote) masked = NULL;
    g_autofree char *stray = g_build_filename(f->admin_dir, "README", NULL);
    g_autofree char *keys_only = g_build_filename(f->admin_dir, "keys-only", "keys", NULL);

    /* nothing is defined yet */
    remotes = mf_remote_list_for_dirs(f->vendor_dir, f->admin_dir, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(remotes->len, ==, 0);
    g_clear_pointer(&remotes, g_ptr_array_unref);

    mft_write_remote(f->vendor_dir, "zeta", "[Remote]\nURL=https://zeta.example.org\n");
    mft_write_remote(f->vendor_dir, "vendor-only", "[Remote]\nURL=https://vendor.example.org\n");
    mft_write_remote(f->vendor_dir, "masked", "[Remote]\nURL=https://masked.example.org\n");
    mft_write_remote(f->admin_dir, "masked", NULL);
    mft_write_remote(f->admin_dir, "admin.only", "[Remote]\nURL=https://admin.example.org\n");
    /* a file is not a remote, and neither is a directory of keys alone */
    mft_make_dirs(f->admin_dir);
    mft_write_file(stray, "not a remote");
    mft_make_dirs(keys_only);

    remotes = mf_remote_list_for_dirs(f->vendor_dir, f->admin_dir, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(remotes->len, ==, 3);
    g_assert_cmpstr(mf_remote_get_id(g_ptr_array_index(remotes, 0)), ==, "admin.only");
    g_assert_cmpstr(mf_remote_get_id(g_ptr_array_index(remotes, 1)), ==, "vendor-only");
    g_assert_cmpstr(mf_remote_get_id(g_ptr_array_index(remotes, 2)), ==, "zeta");

    masked = mf_remote_new_for_dirs(f->vendor_dir, f->admin_dir, NULL, "masked", &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_null(masked);
}

/**
 * test_invalid:
 *
 * A remote with an invalid definition can not be loaded, and the list of
 * remotes leaves it out with a warning.
 */
static void
test_invalid(Fixture *f, gconstpointer user_data)
{
    static const char *const configs[] = {
        /* no URL */
        "[Remote]\n",
        /* a URL that is not a plain https URL with a host */
        "[Remote]\nURL=http://example.org/repo\n",
        "[Remote]\nURL=https://example.org/repo?tag=1\n",
        "[Remote]\nURL=https://example.org/repo#top\n",
        "[Remote]\nURL=https://user@example.org/repo\n",
        "[Remote]\nURL=https:///repo\n",
        /* the reserved channel name, an invalid name, a channel twice, and no channel */
        "[Remote]\nURL=https://example.org\nChannels=local;\n",
        "[Remote]\nURL=https://example.org\nChannels=Beta;\n",
        "[Remote]\nURL=https://example.org\nChannels=main;main;\n",
        "[Remote]\nURL=https://example.org\nChannels=\n",
        "not a key file",
    };
    static const char *const ids[] = {"manifold-local-x", "-x", "a/b", "a b"};

    for (size_t i = 0; i < G_N_ELEMENTS(configs); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(MfRemote) remote = NULL;
        g_autoptr(GPtrArray) remotes = NULL;

        mft_write_remote(f->admin_dir, "example", configs[i]);
        remote = mf_remote_new_for_dirs(f->vendor_dir, f->admin_dir, NULL, "example", &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_FAILED);
        g_assert_null(remote);
        g_clear_error(&error);

        /* the list leaves the remote out with a warning */
        g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "Ignoring the remote example:*");
        remotes = mf_remote_list_for_dirs(f->vendor_dir, f->admin_dir, NULL, &error);
        g_test_assert_expected_messages();
        g_assert_no_error(error);
        g_assert_cmpuint(remotes->len, ==, 0);
    }

    /* an ID that is not a valid name for a remote */
    for (size_t i = 0; i < G_N_ELEMENTS(ids); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(MfRemote)
            remote = mf_remote_new_for_dirs(f->vendor_dir, f->admin_dir, NULL, ids[i], &error);

        g_assert_error(error, MF_ERROR, MF_ERROR_FAILED);
        g_assert_null(remote);
    }
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

#define add_test(name, func) g_test_add("/remote/" name, Fixture, NULL, mft_setup, func, mft_teardown)
    add_test("config-override", test_config_override);
    add_test("trusted-keys", test_trusted_keys);
    add_test("state", test_state);
    add_test("list", test_list);
    add_test("invalid", test_invalid);
#undef add_test

    return g_test_run();
}
