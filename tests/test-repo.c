/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <inttypes.h>
#include <manifold.h>

#include "mf-repo-private.h"
#include "mft-utils.h"

#define ARCH "x86_64"

/* made-up hashes for files that the tests never read */
#define FAKE_SHA256 "1111111111111111111111111111111111111111111111111111111111111111"
#define FAKE_BLAKE3 "2222222222222222222222222222222222222222222222222222222222222222"

static char *test_dir = NULL;
static char *repo_key = NULL;           /* the key that signs the repository */
static char *other_key = NULL;          /* a key nobody trusts */
static MfTrustedKeys *repo_keys = NULL; /* the keys that are trusted to sign the repository */

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
    return g_strdup_printf("\"size\": %" PRIu64 ", \"sha256\": \"%s\", \"blake3\": \"%s\"",
                           entry.size,
                           entry.sha256,
                           entry.blake3);
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
 * mft_read_root_index:
 *
 * Sign @json with @key, and read it as the root index of the repository "example".
 */
static MfRepoRootIndex *
mft_read_root_index(const char *key, const char *json, MfTrustedKeys *keys, GError **error)
{
    g_autoptr(GBytes) data = mft_zstd(json);
    g_autoptr(GBytes) signature = mft_sign(key, MF_SIGNATURE_SCOPE_REPO, data);

    return mf_repo_root_index_new_from_signed(data, signature, keys, "example", error);
}

/**
 * mft_assert_root_index_refused:
 *
 * Check that a root index is refused, although the trusted key signed it.
 */
static void
mft_assert_root_index_refused(const char *json)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRepoRootIndex) index = mft_read_root_index(repo_key, json, repo_keys, &error);

    g_test_message("%.200s", json);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
    g_assert_null(index);
}

/**
 * test_root_index:
 *
 * Read a signed root index, which lists the channels of a repository.
 */
static void
test_root_index(void)
{
    g_autofree char *json = mft_read_sample("repo-index.json");
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRepoRootIndex) index = mft_read_root_index(repo_key, json, repo_keys, &error);
    g_autoptr(GDateTime) before = g_date_time_new_utc(2027, 1, 1, 0, 0, 0);
    g_autoptr(GDateTime) after = g_date_time_new_utc(2027, 6, 1, 0, 0, 0);
    MfRepoChannel *channel;
    MfRepoArchitecture *arch;

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
    g_assert_cmpuint(arch->component_index.size, ==, 2048);
    g_assert_null(mf_repo_channel_find_architecture(channel, "aarch64"));
}

/**
 * test_root_index_refused:
 *
 * A root index is only read if a key that is trusted for its remote signed
 * it, and if its contents are valid.
 */
static void
test_root_index_refused(void)
{
    static const char *const invalid_channels[] = {
        /* the reserved channel name */
        "\"local\": {\"architectures\": {}}",
        /* an architecture without files */
        "\"main\": {\"architectures\": {\"x86_64\": {}}}",
        /* an image URL that is not https, and one with a trailing slash */
        "\"main\": {\"image-url-prefix\": \"http://e.org\", \"architectures\": {}}",
        "\"main\": {\"image-url-prefix\": \"https://e.org/\", \"architectures\": {}}",
        /* a channel without architectures */
        "\"main\": {}",
    };
    g_autofree char *valid = mft_read_sample("repo-index.json");
    g_autofree char *other_id = mft_str_replace(valid, "\"id\": \"example\"", "\"id\": \"other\"");
    g_autofree char *bad_date = mft_str_replace(valid, "2027-05-04T14:57:58Z", "tomorrow");
    g_autofree char *padding = g_strnfill(17 * 1024 * 1024, 'x');
    g_autofree char *huge = g_strdup_printf("{\"id\": \"%s\"}", padding);
    g_autoptr(MfTrustedKeys) no_keys = mf_trusted_keys_new();
    g_autoptr(GError) error = NULL;

    /* a key that is not trusted for the remote, and a remote without keys */
    g_assert_null(mft_read_root_index(other_key, valid, repo_keys, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);
    g_assert_null(mft_read_root_index(repo_key, valid, no_keys, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);

    /* the root index of a different repository */
    mft_assert_root_index_refused(other_id);

    mft_assert_root_index_refused(bad_date);
    for (size_t i = 0; i < G_N_ELEMENTS(invalid_channels); i++) {
        g_autofree char *json = mft_root_index_json("example", "2027-05-04T14:57:58Z", invalid_channels[i]);

        mft_assert_root_index_refused(json);
    }

    /* a root index that is too large is refused, even if it compresses well */
    mft_assert_root_index_refused(huge);
}

/**
 * test_root_index_id:
 *
 * The ID of a root index can be read before its signature is checked, to
 * find the keys of the remote. The ID has to be a valid remote ID.
 */
static void
test_root_index_id(void)
{
    static const char *const invalid[] = {
        "{\"id\": \"../example\"}",
        "{\"id\": \"manifold-local-example\"}",
        "{\"id\": \"\"}",
        "{\"id\": 1}",
        "{\"summary\": \"Example applications\"}",
    };
    g_autofree char *json = mft_read_sample("repo-index.json");
    g_autoptr(GBytes) data = mft_zstd(json);
    g_autoptr(GBytes) not_compressed = g_bytes_new_static("{\"id\": \"example\"}", 17);
    g_autoptr(GError) error = NULL;
    g_autofree char *remote_id = mf_repo_root_index_read_id(data, &error);

    g_assert_no_error(error);
    g_assert_cmpstr(remote_id, ==, "example");

    /* a root index is always compressed */
    g_assert_null(mf_repo_root_index_read_id(not_compressed, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
    g_clear_error(&error);

    /* the ID is used as a file name in the repository and as a directory name in the store */
    for (size_t i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autoptr(GBytes) invalid_data = mft_zstd(invalid[i]);

        g_assert_null(mf_repo_root_index_read_id(invalid_data, &error));
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
        g_clear_error(&error);
    }
}

/**
 * mft_version_json:
 * @location: the location of the manifest in the repository
 *
 * Returns: (transfer full): a version of org.example.App as the component index lists it
 */
static char *
mft_version_json(const char *version, uint64_t revision, const char *location)
{
    /* clang-format off */
    return g_strdup_printf(
        "{\"bundle\": \"" ARCH "/org.example.App/%s\","
        " \"version\": \"%s\","
        " \"revision\": %" PRIu64 ","
        " \"manifest\": {\"location\": \"%s\", \"size\": 10,"
                       " \"sha256\": \"" FAKE_SHA256 "\", \"blake3\": \"" FAKE_BLAKE3 "\"},"
        " \"zck-header\": {\"size\": 2184, \"sha256\": \"" FAKE_SHA256 "\"}}",
        version,
        version,
        revision,
        location);
    /* clang-format on */
}

/**
 * mft_read_component_index:
 * @versions: the versions of org.example.App, as the elements of a JSON list
 *
 * Returns: (transfer full) (nullable): the component index, or %NULL on error
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
 * mft_assert_component_index_refused:
 *
 * Check that a component index with the given versions of org.example.App is refused.
 */
static void
mft_assert_component_index_refused(const char *versions)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRepoComponentIndex) index = mft_read_component_index(versions, &error);

    g_test_message("%s", versions);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
    g_assert_null(index);
}

/**
 * test_component_index:
 *
 * Read a component index, which lists the versions of every component of a channel.
 */
static void
test_component_index(void)
{
    /* a version is made invalid by replacing a part of its JSON */
    static const struct {
        const char *old;
        const char *new;
    } flaws[] = {
        /* a bundle of a different architecture */
        {"x86_64/org.example.App/1.2",      "aarch64/org.example.App/1.2"},
        /* a manifest outside of the repository, and one that is not compressed */
        {"pool/org/example/App/b.json.zst", "../b.json.zst"              },
        {"pool/org/example/App/b.json.zst", "pool/b.json"                },
        /* a manifest that is too large */
        {"\"size\": 10",                    "\"size\": 17825792"         },
    };
    g_autofree char *newer = mft_version_json("1.2", 1, "pool/org/example/App/b.json.zst");
    g_autofree char *older = mft_version_json("1.1", 7, "pool/org/example/App/a.json.zst");
    g_autofree char *both = g_strdup_printf("%s, %s", newer, older);
    g_autofree char *reversed = g_strdup_printf("%s, %s", older, newer);
    g_autofree char *twice = g_strdup_printf("%s, %s", newer, newer);
    g_autoptr(GBytes) data = mft_zstd("{\"components\": {}}");
    g_autoptr(GBytes) other_data = mft_zstd("{\"components\": {} }");
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRepoComponentIndex) index = mft_read_component_index(both, &error);
    g_autofree char *image_location = NULL;
    GPtrArray *versions;
    MfRepoVersion *version;
    MfFileEntry other_entry;

    g_assert_no_error(error);
    versions = mf_repo_component_index_get_versions(index, "org.example.App");
    g_assert_nonnull(versions);
    g_assert_cmpuint(versions->len, ==, 2);
    g_assert_cmpstr(((MfRepoVersion *) g_ptr_array_index(versions, 0))->version, ==, "1.2");
    version = g_ptr_array_index(versions, 1);
    g_assert_cmpuint(version->revision, ==, 7);
    g_assert_cmpstr(version->manifest_location, ==, "pool/org/example/App/a.json.zst");
    g_assert_null(mf_repo_component_index_get_versions(index, "org.example.Other"));

    /* the image of a version is next to its manifest */
    image_location = mf_repo_version_build_image_location(version);
    g_assert_cmpstr(image_location, ==, "pool/org/example/App/a.raw.zck");

    /* a component without versions, versions that are not listed newest first, and a version twice */
    mft_assert_component_index_refused("");
    mft_assert_component_index_refused(reversed);
    mft_assert_component_index_refused(twice);

    for (size_t i = 0; i < G_N_ELEMENTS(flaws); i++) {
        g_autofree char *invalid = mft_str_replace(newer, flaws[i].old, flaws[i].new);

        mft_assert_component_index_refused(invalid);
    }

    /* a file is only read if it matches the size and hash that the root index records for it */
    mf_file_entry_set_for_bytes(&other_entry, other_data);
    g_assert_null(mf_repo_component_index_new(data, &other_entry, ARCH, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
}

/**
 * mft_read_manifest:
 * @manifest: the compressed manifest that the component index records for org.example.App 1.2
 * @served: the compressed manifest that the repository serves
 *
 * Returns: (transfer full) (nullable): the manifest, or %NULL on error
 */
static MfManifest *
mft_read_manifest(GBytes *manifest, GBytes *served, GError **error)
{
    g_autofree char *entry = mft_entry_json(manifest);
    g_autofree char *version_json = g_strdup_printf(
        "{\"bundle\": \"" ARCH
        "/org.example.App/1.2\", "
        "\"version\": \"1.2\", \"revision\": 1, "
        "\"manifest\": {\"location\": \"pool/b.json.zst\", %s}, "
        "\"zck-header\": {\"size\": 2184, \"sha256\": \"" FAKE_SHA256 "\"}}",
        entry);
    g_autoptr(GError) local_error = NULL;
    g_autoptr(MfRepoComponentIndex) index = mft_read_component_index(version_json, &local_error);
    MfRepoVersion *version;

    g_assert_no_error(local_error);
    version = g_ptr_array_index(mf_repo_component_index_get_versions(index, "org.example.App"), 0);

    return mf_repo_version_read_manifest(version, served, "org.example.App", ARCH, error);
}

/**
 * test_manifest:
 *
 * A manifest of a repository has to match its entry in the component index.
 */
static void
test_manifest(void)
{
    const MftTestManifest listed = {.component_id = "org.example.App", .kind = "core", .version = "1.2"};
    /* manifests that describe a different component, version, revision or architecture */
    const MftTestManifest others[] = {
        {.component_id = "org.example.Other", .kind = "core", .version = "1.2"},
        {.component_id = "org.example.App", .kind = "core", .version = "1.3"},
        {.component_id = "org.example.App", .kind = "core", .version = "1.2", .revision = 2},
        {.component_id = "org.example.App", .kind = "core", .version = "1.2", .architecture = "aarch64"},
    };
    g_autofree char *json = mft_manifest_json(&listed);
    g_autofree char *changed_json = g_strconcat(json, " ", NULL);
    g_autoptr(GBytes) data = mft_zstd(json);
    g_autoptr(GBytes) changed = mft_zstd(changed_json);
    g_autoptr(GBytes) not_a_manifest = mft_zstd("{}");
    g_autoptr(GError) error = NULL;
    g_autoptr(MfManifest) manifest = mft_read_manifest(data, data, &error);

    g_assert_no_error(error);
    g_assert_nonnull(manifest);
    g_assert_cmpstr(mf_manifest_get_component_id(manifest), ==, "org.example.App");
    /* the data of the manifest is the decompressed file, byte for byte */
    g_assert_cmpmem(g_bytes_get_data(mf_manifest_get_data(manifest), NULL),
                    g_bytes_get_size(mf_manifest_get_data(manifest)),
                    json,
                    strlen(json));

    /* a served manifest that does not match the hash that the component index records */
    g_assert_null(mft_read_manifest(data, changed, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);

    /* a manifest that matches the hash, but not the entry of the component index */
    for (size_t i = 0; i < G_N_ELEMENTS(others); i++) {
        g_autofree char *other_json = mft_manifest_json(&others[i]);
        g_autoptr(GBytes) other = mft_zstd(other_json);

        g_assert_null(mft_read_manifest(other, other, &error));
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
        g_clear_error(&error);
    }

    /* a file that matches the hash, but is not a manifest */
    g_assert_null(mft_read_manifest(not_a_manifest, not_a_manifest, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
}

/**
 * mft_read_icon_index:
 * @path: the path of an icon in the icon directory of the catalog
 * @location: the location of the icon in the repository
 *
 * Returns: (transfer full) (nullable): an icon index with the icon and a second
 *   icon of 48x48 pixels, or %NULL on error
 */
static GPtrArray *
mft_read_icon_index(const char *path, const char *location, uint64_t size, GError **error)
{
    /* clang-format off */
    g_autofree char *json = g_strdup_printf(
        "{\"%s\": {\"location\": \"%s\", \"size\": %" PRIu64 ","
                 " \"sha256\": \"" FAKE_SHA256 "\", \"blake3\": \"" FAKE_BLAKE3 "\"},"
        " \"48x48/org.example.App.jxl\": {\"location\": \"media/48.jxl\", \"size\": 10,"
                 " \"sha256\": \"" FAKE_SHA256 "\", \"blake3\": \"" FAKE_BLAKE3 "\"}}",
        path,
        location,
        size);
    /* clang-format on */
    g_autoptr(GBytes) data = mft_zstd(json);
    MfFileEntry entry;

    mf_file_entry_set_for_bytes(&entry, data);
    return mf_repo_icon_index_new(data, &entry, error);
}

/**
 * test_icon_index:
 *
 * Read an icon index, which lists the icons of the catalog of a channel.
 */
static void
test_icon_index(void)
{
    static const struct {
        const char *path;
        const char *location;
    } invalid[] = {
        /* paths that are not "<size>/<name>" */
        {"64x64/../org.example.App.jxl", "media/a.jxl"   },
        {"64/org.example.App.jxl",       "media/a.jxl"   },
        {"64x64@1/org.example.App.jxl",  "media/a.jxl"   },
        {"64x64/.org.example.App.jxl",   "media/a.jxl"   },
        {"org.example.App.jxl",          "media/a.jxl"   },
        /* locations outside of the repository */
        {"64x64/org.example.App.jxl",    "/media/a.jxl"  },
        {"64x64/org.example.App.jxl",    "media/../a.jxl"},
    };
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray)
        icons = mft_read_icon_index("64x64@2/org.example.App.jxl", "media/64.jxl", 10, &error);

    g_assert_no_error(error);
    g_assert_cmpuint(icons->len, ==, 2);
    /* the icons are ordered by their path */
    g_assert_cmpstr(((MfRepoIcon *) g_ptr_array_index(icons, 0))->path, ==, "48x48/org.example.App.jxl");
    g_assert_cmpstr(((MfRepoIcon *) g_ptr_array_index(icons, 1))->location, ==, "media/64.jxl");

    for (size_t i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_test_message("%s %s", invalid[i].path, invalid[i].location);
        g_assert_null(mft_read_icon_index(invalid[i].path, invalid[i].location, 10, &error));
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
        g_clear_error(&error);
    }

    /* an icon of more than 2 MiB */
    g_assert_null(
        mft_read_icon_index("64x64/org.example.App.jxl", "media/a.jxl", 2 * 1024 * 1024 + 1, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY);
}

/**
 * mft_check_catalog:
 * @origin: the origin of the catalog
 * @bundle: (nullable): the value of the `bundle` element of org.example.App
 *
 * Returns: whether the catalog is accepted for the channel "main" of the repository "example"
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
 *
 * The catalog of a channel has to describe versions that the component index lists.
 */
static void
test_catalog(void)
{
    g_autofree char *version = mft_version_json("1.2", 1, "pool/b.json.zst");
    g_autoptr(GError) error = NULL;
    g_autoptr(MfRepoComponentIndex) index = mft_read_component_index(version, &error);

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
    g_autofree char *public_key_path = NULL;
    g_autofree char *public_key = NULL;
    g_autoptr(GError) error = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);

    /* the keys of the repository, which all tests share */
    test_dir = mft_make_tmp_dir("repo");
    repo_key = mft_make_key(test_dir, "repo");
    other_key = mft_make_key(test_dir, "other");
    public_key_path = g_strconcat(repo_key, ".pub", NULL);
    public_key = mft_read_file(public_key_path);
    repo_keys = mf_trusted_keys_new();
    g_assert_true(mf_trusted_keys_add_key(repo_keys, public_key, &error));
    g_assert_no_error(error);

    g_test_add_func("/repo/root-index", test_root_index);
    g_test_add_func("/repo/root-index-refused", test_root_index_refused);
    g_test_add_func("/repo/root-index-id", test_root_index_id);
    g_test_add_func("/repo/component-index", test_component_index);
    g_test_add_func("/repo/manifest", test_manifest);
    g_test_add_func("/repo/icon-index", test_icon_index);
    g_test_add_func("/repo/catalog", test_catalog);

    ret = g_test_run();
    g_clear_object(&repo_keys);
    mft_rm_rf(test_dir);

    return ret;
}
