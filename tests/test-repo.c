/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <zstd.h>
#include <manifold.h>

#include "mf-repo-private.h"
#include "mft-utils.h"

#define ARCH "x86_64"

typedef struct {
    char *dir;
    char *key;       /* the key that signs the repository */
    char *other_key; /* a key nobody trusts */
    MfTrustedKeys *keys;
} Fixture;

static void
mft_setup(Fixture *f, gconstpointer user_data)
{
    g_autofree char *ssh_keygen = g_find_program_in_path("ssh-keygen");
    g_autofree char *public_key = NULL;
    g_autofree char *public_key_path = NULL;
    g_autoptr(GError) error = NULL;

    f->dir = mft_make_tmp_dir("repo");
    f->keys = mf_trusted_keys_new();
    if (ssh_keygen == NULL)
        return;
    f->key = mft_make_key(f->dir, "repo");
    f->other_key = mft_make_key(f->dir, "other");
    public_key_path = g_strconcat(f->key, ".pub", NULL);
    public_key = mft_read_file(public_key_path);
    g_assert_true(mf_trusted_keys_add_key(f->keys, public_key, &error));
    g_assert_no_error(error);
}

static void
mft_teardown(Fixture *f, gconstpointer user_data)
{
    mft_rm_rf(f->dir);
    g_free(f->dir);
    g_free(f->key);
    g_free(f->other_key);
    g_object_unref(f->keys);
}

/**
 * mft_zstd:
 *
 * Returns: (transfer full): @text, compressed with zstd
 */
static GBytes *
mft_zstd(const char *text)
{
    gsize bound = ZSTD_compressBound(strlen(text));
    g_autofree guint8 *out = g_malloc(bound);
    size_t size = ZSTD_compress(out, bound, text, strlen(text), 3);

    g_assert_false(ZSTD_isError(size));
    return g_bytes_new(out, size);
}

/**
 * mft_entry_json:
 *
 * Returns: (transfer full): the file entry of @data as JSON members
 */
static char *
mft_entry_json(GBytes *data)
{
    MfFileEntry entry;

    mf_file_entry_set_for_bytes(&entry, data);
    return g_strdup_printf("\"size\": %" G_GUINT64_FORMAT ", \"sha256\": \"%s\", \"blake3\": \"%s\"",
                           entry.size,
                           entry.sha256,
                           entry.blake3);
}

/**
 * mft_sign:
 *
 * Returns: (transfer full): a repository signature over @data, made with the key at @key
 */
static GBytes *
mft_sign(const char *key, GBytes *data)
{
    g_autoptr(MfSigner) signer = mf_signer_new_for_key_file(key);
    g_autoptr(GError) error = NULL;
    GBytes *signature = mf_signer_sign(signer, MF_SIGNATURE_SCOPE_REPO, data, &error);

    g_assert_no_error(error);
    return signature;
}

/**
 * mft_root_index_json:
 * @channels: the channels, as the members of the JSON object
 *
 * Returns: (transfer full): a root index of the repository @id, as JSON
 */
static char *
mft_root_index_json(const char *id, const char *valid_until, const char *channels)
{
    return g_strdup_printf(
        "{\"id\": \"%s\", \"summary\": \"Example applications\", "
        "\"publisher\": {\"name\": \"Example Inc.\", \"email\": \"repo@example.org\"}, "
        "\"serial\": 1793635200, \"date\": \"2026-11-02T18:00:00Z\", "
        "\"valid-until\": \"%s\", \"channels\": {%s}}",
        id,
        valid_until,
        channels);
}

/**
 * mft_channel_json:
 *
 * Returns: (transfer full): a channel as JSON, which lists the same made-up file for all three entries
 */
static char *
mft_channel_json(const char *name, const char *prefix)
{
    g_autoptr(GBytes) file = mft_zstd("{}");
    g_autofree char *entry = mft_entry_json(file);
    g_autofree char *prefix_member = prefix != NULL
                                         ? g_strdup_printf("\"image-url-prefix\": \"%s\", ", prefix)
                                         : g_strdup("");

    return g_strdup_printf(
        "\"%s\": {%s\"architectures\": {\"" ARCH
        "\": {"
        "\"index.json.zst\": {%s}, \"components.xml.zst\": {%s}, \"icons.json.zst\": {%s}}}}",
        name,
        prefix_member,
        entry,
        entry,
        entry);
}

/**
 * mft_read_root_index:
 *
 * Sign @json with @key, and read it back with the keys of the fixture.
 */
static MfRepoRootIndex *
mft_read_root_index(Fixture *f, const char *key, const char *json, GError **error)
{
    g_autoptr(GBytes) data = mft_zstd(json);
    g_autoptr(GBytes) signature = mft_sign(key, data);

    return mf_repo_root_index_new_from_signed(data, signature, f->keys, "example", error);
}

/**
 * test_root_index:
 */
static void
test_root_index(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRepoRootIndex) index = NULL;
    g_autoptr(GDateTime) before = g_date_time_new_utc(2027, 1, 1, 0, 0, 0);
    g_autoptr(GDateTime) after = g_date_time_new_utc(2027, 6, 1, 0, 0, 0);
    g_autofree char *channel_json = NULL;
    g_autofree char *json = NULL;
    MfRepoChannel *channel;
    MfRepoArchitecture *arch;

    if (f->key == NULL) {
        g_test_skip("ssh-keygen is not installed");
        return;
    }

    channel_json = mft_channel_json("main", "https://example.org/images");
    json = mft_root_index_json("example", "2027-05-04T14:57:58Z", channel_json);
    index = mft_read_root_index(f, f->key, json, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(index->id, ==, "example");
    g_assert_cmpstr(index->summary, ==, "Example applications");
    g_assert_cmpstr(index->publisher_name, ==, "Example Inc.");
    g_assert_cmpstr(index->publisher_email, ==, "repo@example.org");
    g_assert_cmpuint(index->serial, ==, 1793635200);
    g_assert_nonnull(index->key_fingerprint);
    g_assert_false(mf_repo_root_index_is_expired(index, before));
    g_assert_true(mf_repo_root_index_is_expired(index, after));

    channel = mf_repo_root_index_find_channel(index, "main");
    g_assert_nonnull(channel);
    g_assert_cmpstr(channel->image_url_prefix, ==, "https://example.org/images");
    g_assert_null(mf_repo_root_index_find_channel(index, "beta"));
    arch = mf_repo_channel_find_architecture(channel, ARCH);
    g_assert_nonnull(arch);
    g_assert_cmpuint(arch->component_index.size, >, 0);
    g_assert_null(mf_repo_channel_find_architecture(channel, "aarch64"));
}

/**
 * test_root_index_refused:
 */
static void
test_root_index_refused(Fixture *f, gconstpointer user_data)
{
    g_autofree char *channel_json = NULL;
    g_autofree char *valid = NULL;
    g_autoptr(GString) huge = g_string_new("{\"id\": \"");
    const struct {
        const char *channels;
        const char *valid_until;
    } invalid[] = {
        {"\"local\": {\"architectures\": {}}",                                          "2027-05-04T14:57:58Z"},
        {"\"main\": {\"architectures\": {\"x86_64\": {}}}",                             "2027-05-04T14:57:58Z"},
        {"\"main\": {\"image-url-prefix\": \"http://e.org\", \"architectures\": {}}",   "2027-05-04T14:57:58Z"},
        {"\"main\": {\"image-url-prefix\": \"https://e.org/\", \"architectures\": {}}",
         "2027-05-04T14:57:58Z"                                                                               },
        {"\"main\": {}",                                                                "2027-05-04T14:57:58Z"},
        {"",                                                                            "tomorrow"            },
    };

    if (f->key == NULL) {
        g_test_skip("ssh-keygen is not installed");
        return;
    }
    channel_json = mft_channel_json("main", NULL);
    valid = mft_root_index_json("example", "2027-05-04T14:57:58Z", channel_json);

    /* only keys trusted for this remote count, and the index must be of this repository */
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(MfRepoRootIndex) index = mft_read_root_index(f, f->other_key, valid, &error);

        g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
        g_assert_null(index);
    }
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(MfTrustedKeys) none = mf_trusted_keys_new();
        g_autoptr(GBytes) data = mft_zstd(valid);
        g_autoptr(GBytes) signature = mft_sign(f->key, data);
        g_autoptr(MfRepoRootIndex)
            index = mf_repo_root_index_new_from_signed(data, signature, none, "example", &error);

        g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
        g_assert_null(index);
    }
    {
        g_autoptr(GError) error = NULL;
        g_autofree char *other = mft_root_index_json("other", "2027-05-04T14:57:58Z", channel_json);
        g_autoptr(MfRepoRootIndex) index = mft_read_root_index(f, f->key, other, &error);

        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
        g_assert_null(index);
    }

    for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autoptr(GError) error = NULL;
        g_autofree char *json = mft_root_index_json("example", invalid[i].valid_until, invalid[i].channels);
        g_autoptr(MfRepoRootIndex) index = mft_read_root_index(f, f->key, json, &error);

        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
        g_assert_null(index);
    }

    /* a root index that is too large is refused, even if it compresses well */
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(MfRepoRootIndex) index = NULL;

        for (gsize i = 0; i < 17 * 1024 * 1024; i++)
            g_string_append_c(huge, 'x');
        g_string_append(huge, "\"}");
        index = mft_read_root_index(f, f->key, huge->str, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
        g_assert_null(index);
    }
}

/**
 * mft_version_json:
 *
 * Returns: (transfer full): a version of org.example.App as the component index lists it
 */
static char *
mft_version_json(const char *bundle,
                 const char *version,
                 guint64 revision,
                 const char *location,
                 guint64 size)
{
    return g_strdup_printf("{\"bundle\": \"%s\", \"version\": \"%s\", \"revision\": %" G_GUINT64_FORMAT
                           ", "
                           "\"manifest\": {\"location\": \"%s\", \"size\": %" G_GUINT64_FORMAT
                           ", "
                           "\"sha256\": \"%064d\", \"blake3\": \"%064d\"}, "
                           "\"zck-header\": {\"size\": 2184, \"sha256\": \"%064d\"}}",
                           bundle,
                           version,
                           revision,
                           location,
                           size,
                           1,
                           2,
                           3);
}

/**
 * mft_read_component_index:
 */
static MfRepoComponentIndex *
mft_read_component_index(const char *versions, GError **error)
{
    g_autofree char *json = g_strdup_printf("{\"components\": {\"org.example.App\": {\"versions\": [%s]}}}",
                                            versions);
    g_autoptr(GBytes) data = mft_zstd(json);
    MfFileEntry entry;

    mf_file_entry_set_for_bytes(&entry, data);
    return mf_repo_component_index_new(data, &entry, ARCH, error);
}

/**
 * test_component_index:
 */
static void
test_component_index(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRepoComponentIndex) index = NULL;
    g_autofree char *newer = mft_version_json("x86_64/org.example.App/1.2",
                                              "1.2",
                                              1,
                                              "pool/org/example/App/b.json.zst",
                                              10);
    g_autofree char *older = mft_version_json("x86_64/org.example.App/1.1",
                                              "1.1",
                                              7,
                                              "pool/org/example/App/a.json.zst",
                                              10);
    g_autofree char *both = g_strdup_printf("%s, %s", newer, older);
    g_autofree char *reversed = g_strdup_printf("%s, %s", older, newer);
    g_autofree char *twice = g_strdup_printf("%s, %s", newer, newer);
    g_autofree char *other_arch = mft_version_json("aarch64/org.example.App/1.2",
                                                   "1.2",
                                                   1,
                                                   "pool/b.json.zst",
                                                   10);
    g_autofree char *escapes = mft_version_json("x86_64/org.example.App/1.2", "1.2", 1, "../b.json.zst", 10);
    g_autofree char *no_suffix = mft_version_json("x86_64/org.example.App/1.2", "1.2", 1, "pool/b.json", 10);
    g_autofree char *huge = mft_version_json("x86_64/org.example.App/1.2",
                                             "1.2",
                                             1,
                                             "pool/b.json.zst",
                                             17 * 1024 * 1024);
    const char *invalid[] = {"", reversed, twice, other_arch, escapes, no_suffix, huge};
    GPtrArray *versions;

    index = mft_read_component_index(both, &error);
    g_assert_no_error(error);
    versions = mf_repo_component_index_get_versions(index, "org.example.App");
    g_assert_nonnull(versions);
    g_assert_cmpuint(versions->len, ==, 2);
    g_assert_cmpstr(((MfRepoVersion *) g_ptr_array_index(versions, 0))->version, ==, "1.2");
    g_assert_cmpstr(((MfRepoVersion *) g_ptr_array_index(versions, 1))->manifest_location,
                    ==,
                    "pool/org/example/App/a.json.zst");
    g_assert_cmpuint(((MfRepoVersion *) g_ptr_array_index(versions, 1))->revision, ==, 7);
    g_assert_null(mf_repo_component_index_get_versions(index, "org.example.Other"));

    for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autoptr(GError) local_error = NULL;
        g_autoptr(MfRepoComponentIndex) refused = mft_read_component_index(invalid[i], &local_error);

        g_assert_error(local_error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
        g_assert_null(refused);
    }

    /* a file is only read if it matches the size and hash that the root index records for it */
    {
        g_autoptr(GBytes) data = mft_zstd("{\"components\": {}}");
        g_autoptr(GBytes) other = mft_zstd("{\"components\": {} }");
        g_autoptr(MfRepoComponentIndex) refused = NULL;
        MfFileEntry entry;

        mf_file_entry_set_for_bytes(&entry, other);
        refused = mf_repo_component_index_new(data, &entry, ARCH, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
        g_assert_null(refused);
    }
}

/**
 * mft_read_icon_index:
 */
static GPtrArray *
mft_read_icon_index(const char *path, const char *location, guint64 size, GError **error)
{
    g_autofree char *json = g_strdup_printf(
        "{\"%s\": {\"location\": \"%s\", \"size\": %" G_GUINT64_FORMAT
        ", \"sha256\": \"%064d\", \"blake3\": \"%064d\"},"
        " \"48x48/org.example.App.jxl\": {\"location\": \"media/48.jxl\", \"size\": 10, \"sha256\": \"%064d\","
        " \"blake3\": \"%064d\"}}",
        path,
        location,
        size,
        1,
        2,
        3,
        4);
    g_autoptr(GBytes) data = mft_zstd(json);
    MfFileEntry entry;

    mf_file_entry_set_for_bytes(&entry, data);
    return mf_repo_icon_index_new(data, &entry, error);
}

/**
 * test_icon_index:
 */
static void
test_icon_index(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) icons = NULL;
    const struct {
        const char *path;
        const char *location;
        guint64 size;
    } invalid[] = {
        {"64x64/../org.example.App.jxl", "media/a.jxl",    10                 },
        {"64/org.example.App.jxl",       "media/a.jxl",    10                 },
        {"64x64@1/org.example.App.jxl",  "media/a.jxl",    10                 },
        {"64x64/.org.example.App.jxl",   "media/a.jxl",    10                 },
        {"org.example.App.jxl",          "media/a.jxl",    10                 },
        {"64x64/org.example.App.jxl",    "/media/a.jxl",   10                 },
        {"64x64/org.example.App.jxl",    "media/../a.jxl", 10                 },
        {"64x64/org.example.App.jxl",    "media/a.jxl",    2 * 1024 * 1024 + 1},
    };

    icons = mft_read_icon_index("64x64@2/org.example.App.jxl", "media/64.jxl", 10, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(icons->len, ==, 2);
    /* the icons are ordered by their path */
    g_assert_cmpstr(((MfRepoIcon *) g_ptr_array_index(icons, 0))->path, ==, "48x48/org.example.App.jxl");
    g_assert_cmpstr(((MfRepoIcon *) g_ptr_array_index(icons, 1))->location, ==, "media/64.jxl");

    for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autoptr(GError) local_error = NULL;
        g_autoptr(GPtrArray) refused = mft_read_icon_index(invalid[i].path,
                                                           invalid[i].location,
                                                           invalid[i].size,
                                                           &local_error);

        g_assert_error(local_error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
        g_assert_null(refused);
    }
}

/**
 * mft_check_catalog:
 */
static gboolean
mft_check_catalog(MfRepoComponentIndex *index, const char *origin, const char *bundle, GError **error)
{
    g_autofree char *component = mft_catalog_component_xml("org.example.App", bundle, NULL);
    g_autofree char *xml = g_strdup_printf("<components version=\"1.2\" origin=\"%s\">%s</components>",
                                           origin,
                                           component);
    g_autoptr(GBytes) data = mft_zstd(xml);
    MfFileEntry entry;

    mf_file_entry_set_for_bytes(&entry, data);
    return mf_repo_check_catalog(data, &entry, "example-main", index, error);
}

/**
 * test_catalog:
 */
static void
test_catalog(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRepoComponentIndex) index = NULL;
    g_autofree char *version = mft_version_json("x86_64/org.example.App/1.2",
                                                "1.2",
                                                1,
                                                "pool/b.json.zst",
                                                10);

    index = mft_read_component_index(version, &error);
    g_assert_no_error(error);

    g_assert_true(mft_check_catalog(index, "example-main", "x86_64/org.example.App/1.2", &error));
    g_assert_no_error(error);

    /* a catalog has the origin of its channel */
    g_assert_false(mft_check_catalog(index, "other-main", "x86_64/org.example.App/1.2", &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
    g_clear_error(&error);

    /* every component in a catalog is a version that the component index lists */
    g_assert_false(mft_check_catalog(index, "example-main", "x86_64/org.example.App/1.1", &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
    g_clear_error(&error);
    g_assert_false(mft_check_catalog(index, "example-main", NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

#define add_test(name, func) g_test_add("/repo/" name, Fixture, NULL, mft_setup, func, mft_teardown)
    add_test("root-index", test_root_index);
    add_test("root-index-refused", test_root_index_refused);
    add_test("component-index", test_component_index);
    add_test("icon-index", test_icon_index);
    add_test("catalog", test_catalog);
#undef add_test

    return g_test_run();
}
