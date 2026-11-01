/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>
#include <sys/stat.h>
#include <blake3.h>
#include <manifold.h>

#include "mf-store-private.h"
#include "mfd-image-reader.h"
#include "mft-utils.h"

#define APP_DIR      "/app/share/applications/"
#define MAIN_ENTRY   APP_DIR "com.example.app.desktop"
#define MAIN_CONTENT "[Desktop Entry]\nType=Application\nExec=app\n"

/* the name of the image in the store */
#define VERITY_HASH "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/* where the filesystem starts in the image, as in an image with partitions */
constexpr guint64 DATA_OFFSET = 8192;

constexpr gsize LARGE_FILE_SIZE = 7 * 1024 * 1024;
constexpr gsize TOO_LARGE_FILE_SIZE = 9 * 1024 * 1024;

static char *store_dir = NULL;
static MfStore *store = NULL;

/**
 * mft_make_data:
 *
 * Make data that does not compress, as the tool takes a second to decompress
 * a large file of zeros. Smaller sizes are the start of larger ones.
 *
 * Returns: (transfer full): @size bytes
 */
static char *
mft_make_data(gsize size)
{
    g_autoptr(GRand) rand = g_rand_new_with_seed(1);
    guint32 *words = g_malloc(size + sizeof(guint32));

    for (gsize i = 0; i <= size / sizeof(guint32); i++)
        words[i] = g_rand_int(rand);

    return (char *) words;
}

/**
 * mft_write_sized_file:
 */
static void
mft_write_sized_file(const char *path, gsize size)
{
    g_autofree char *content = mft_make_data(size);
    g_autoptr(GError) error = NULL;

    g_assert_true(g_file_set_contents(path, content, (gssize) size, &error));
    g_assert_no_error(error);
}

/**
 * mft_ensure_image:
 *
 * Make a store that holds a real image, made from the tree of an application,
 * with the filesystem at an offset.
 */
static void
mft_ensure_image(void)
{
    g_autofree char *mkfs = g_find_program_in_path("mkfs.erofs");
    g_autofree char *dump = g_find_program_in_path("dump.erofs");
    g_autofree char *tree = NULL;
    g_autofree char *apps = NULL;
    g_autofree char *main_entry = NULL;
    g_autofree char *large = NULL;
    g_autofree char *too_large = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *link = NULL;
    g_autofree char *filesystem = NULL;
    g_autofree char *content = NULL;
    g_autofree char *image = NULL;
    g_autofree char *image_path = NULL;
    g_autofree char *image_dir = NULL;
    g_autoptr(GError) error = NULL;
    gsize size;

    g_assert_nonnull(mkfs);
    g_assert_nonnull(dump);

    store_dir = mft_make_tmp_dir("mf-test-image-XXXXXX");
    tree = g_build_filename(store_dir, "tree", NULL);
    apps = g_build_filename(tree, APP_DIR, NULL);
    mft_make_dirs(apps);

    main_entry = g_build_filename(tree, MAIN_ENTRY, NULL);
    mft_write_file(main_entry, MAIN_CONTENT);
    large = g_build_filename(apps, "com.example.app.large.desktop", NULL);
    mft_write_sized_file(large, LARGE_FILE_SIZE);
    too_large = g_build_filename(apps, "com.example.app.too-large.desktop", NULL);
    mft_write_sized_file(too_large, TOO_LARGE_FILE_SIZE);
    directory = g_build_filename(apps, "com.example.app.dir.desktop", NULL);
    mft_make_dirs(directory);
    link = g_build_filename(apps, "com.example.app.link.desktop", NULL);
    mft_make_link("/etc/hostname", link);

    filesystem = g_build_filename(store_dir, "filesystem", NULL);
    {
        const char *const argv[] = {"mkfs.erofs", "-zlz4", "--all-root", "--quiet", filesystem, tree, NULL};
        mft_run(NULL, argv);
    }

    g_assert_true(g_file_get_contents(filesystem, &content, &size, &error));
    g_assert_no_error(error);
    image = g_malloc0(DATA_OFFSET + size);
    memcpy(image + DATA_OFFSET, content, size);
    image_path = mf_store_image_path_at(store_dir, VERITY_HASH);
    image_dir = g_path_get_dirname(image_path);
    mft_make_dirs(image_dir);
    g_assert_true(g_file_set_contents(image_path, image, (gssize) (DATA_OFFSET + size), &error));
    g_assert_no_error(error);

    store = mf_store_new_for_path(store_dir);
}

/**
 * mft_blake3:
 *
 * Returns: (transfer full): the hex encoded hash of @size bytes of @data
 */
static char *
mft_blake3(const void *data, gsize size)
{
    guint8 hash[BLAKE3_OUT_LEN];
    blake3_hasher hasher;
    GString *hex = g_string_new(NULL);

    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, size);
    blake3_hasher_finalize(&hasher, hash, sizeof(hash));
    for (gsize i = 0; i < sizeof(hash); i++)
        g_string_append_printf(hex, "%02x", hash[i]);

    return g_string_free(hex, FALSE);
}

/**
 * mft_manifest_new:
 * @entries: the entries of the desktop-entry list, as JSON
 * @verity_hash: the image the manifest describes
 *
 * Returns: (transfer full): a manifest with the files of @entries
 */
static MfManifest *
mft_manifest_new(const char *entries, const char *verity_hash)
{
    g_autofree char *section = g_strdup_printf("{\"desktop-entry\": [%s]}", entries);
    MftTestManifest m = {
        .component_id = "com.example.app",
        .kind = "app",
        .version = "1.0",
        .base_id = "net.example.core",
        .integration = section,
        .verity_root_hash = verity_hash,
        .data_offset = DATA_OFFSET,
    };
    g_autofree char *json = mft_manifest_json(&m);
    g_autoptr(GError) error = NULL;
    MfManifest *manifest = mf_manifest_new_from_data(json, -1, &error);

    g_assert_no_error(error);
    return manifest;
}

/**
 * mft_entry_new:
 * @path: where the file is in the image
 * @data: what we say it holds
 *
 * Returns: (transfer full): an entry of the manifest
 */
static char *
mft_entry_new(const char *path, const void *data, gsize size)
{
    g_autofree char *sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, size);
    g_autofree char *blake3 = mft_blake3(data, size);

    return g_strdup_printf("{\"path\": \"%s\", \"sha256\": \"%s\", \"blake3\": \"%s\"}",
                           path,
                           sha256,
                           blake3);
}

/**
 * mft_read:
 * @entries: the entries of the desktop-entry list, as JSON
 *
 * Returns: (transfer full) (nullable): what the reader returned
 */
static GHashTable *
mft_read(const char *entries, GError **error)
{
    g_autoptr(MfManifest) manifest = mft_manifest_new(entries, VERITY_HASH);

    return mfd_image_read_integration(store, manifest, error);
}

/**
 * mft_read_with_fake_tool:
 * @script: (nullable): a shell script to run as the tool, or %NULL to have none at all
 *
 * Read a file with another tool in the PATH.
 *
 * Returns: (transfer full) (nullable): what the reader returned
 */
static GHashTable *
mft_read_with_fake_tool(const char *script, GError **error)
{
    g_autofree char *dir = mft_make_tmp_dir("mf-test-tool-XXXXXX");
    g_autofree char *tool = g_build_filename(dir, "dump.erofs", NULL);
    g_autofree char *path = g_strdup(g_getenv("PATH"));
    g_autofree char *entry = mft_entry_new(MAIN_ENTRY, MAIN_CONTENT, strlen(MAIN_CONTENT));
    GHashTable *contents;

    if (script != NULL) {
        g_autofree char *text = g_strconcat("#!/bin/sh\n", script, NULL);

        mft_write_file(tool, text);
        g_assert_cmpint(chmod(tool, 0755), ==, 0);
    }

    g_setenv("PATH", dir, TRUE);
    contents = mft_read(entry, error);
    g_setenv("PATH", path, TRUE);
    mft_rm_rf(dir);

    return contents;
}

static void
test_read_files(void)
{
    g_autofree char *data = NULL;
    g_autofree char *main_entry = NULL;
    g_autofree char *large = NULL;
    g_autofree char *entries = NULL;
    g_autoptr(GHashTable) contents = NULL;
    g_autoptr(GError) error = NULL;
    GBytes *content;

    data = mft_make_data(LARGE_FILE_SIZE);
    main_entry = mft_entry_new(MAIN_ENTRY, MAIN_CONTENT, strlen(MAIN_CONTENT));
    large = mft_entry_new(APP_DIR "com.example.app.large.desktop", data, LARGE_FILE_SIZE);
    entries = g_strdup_printf("%s, %s", main_entry, large);

    contents = mft_read(entries, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(g_hash_table_size(contents), ==, 2);

    content = g_hash_table_lookup(contents, MAIN_ENTRY);
    g_assert_nonnull(content);
    g_assert_cmpmem(g_bytes_get_data(content, NULL),
                    g_bytes_get_size(content),
                    MAIN_CONTENT,
                    strlen(MAIN_CONTENT));
    content = g_hash_table_lookup(contents, APP_DIR "com.example.app.large.desktop");
    g_assert_nonnull(content);
    g_assert_cmpuint(g_bytes_get_size(content), ==, LARGE_FILE_SIZE);
}

static void
test_read_nothing(void)
{
    g_autoptr(GHashTable) contents = NULL;
    g_autoptr(GError) error = NULL;

    /* a file can not be read without dump.erofs */
    contents = mft_read_with_fake_tool(NULL, &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_FAILED);
    g_assert_null(contents);
    g_clear_error(&error);

    /* a manifest without files needs neither dump.erofs nor an image in the store */
    {
        g_autofree char *other_image = mft_fake_hash("other image");
        g_autoptr(MfManifest) manifest = mft_manifest_new("", other_image);

        contents = mfd_image_read_integration(store, manifest, &error);
        g_assert_no_error(error);
        g_assert_cmpuint(g_hash_table_size(contents), ==, 0);
    }
}

static void
test_read_not_in_store(void)
{
    g_autofree char *entry = mft_entry_new(MAIN_ENTRY, MAIN_CONTENT, strlen(MAIN_CONTENT));
    g_autofree char *other_image = mft_fake_hash("other image");
    g_autoptr(MfManifest) manifest = mft_manifest_new(entry, other_image);
    g_autoptr(GHashTable) contents = NULL;
    g_autoptr(GError) error = NULL;

    /* only images in the store are verified, so we must not read anything else */
    contents = mfd_image_read_integration(store, manifest, &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_null(contents);
}

static void
test_read_refused(void)
{
    /* dump.erofs exits with 0 for all files in this table, so only the checksums can tell */
    static const struct {
        const char *path;
        const char *claimed;
    } files[] = {
        {MAIN_ENTRY,                                "something else"},
        {MAIN_ENTRY,                                ""              },
        {APP_DIR "com.example.app.missing.desktop", MAIN_CONTENT    },
        {APP_DIR "com.example.app.missing.desktop", ""              },
        {APP_DIR "com.example.app.dir.desktop",     ""              },
        {APP_DIR "com.example.app.link.desktop",    "localhost\n"   },
        {APP_DIR "com.example.app.link.desktop",    ""              },
        {"/app/../../etc/hostname",                 ""              },
    };

    for (gsize i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree char *entry = mft_entry_new(files[i].path, files[i].claimed, strlen(files[i].claimed));
        g_autoptr(GHashTable) contents = NULL;
        g_autoptr(GError) error = NULL;

        g_test_message("%s", files[i].path);
        contents = mft_read(entry, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
        g_assert_null(contents);
    }
}

static void
test_read_limits(void)
{
    g_autofree char *data = NULL;
    g_autoptr(GHashTable) contents = NULL;
    g_autoptr(GError) error = NULL;

    /* a single file that is too large */
    data = mft_make_data(TOO_LARGE_FILE_SIZE);
    {
        g_autofree char *entry = mft_entry_new(APP_DIR "com.example.app.too-large.desktop",
                                               data,
                                               TOO_LARGE_FILE_SIZE);

        contents = mft_read(entry, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(contents);
        g_clear_error(&error);
    }

    /* files that fit one by one, but not together */
    {
        g_autofree char *entry = mft_entry_new(APP_DIR "com.example.app.large.desktop",
                                               data,
                                               LARGE_FILE_SIZE);
        g_autoptr(GString) entries = g_string_new(entry);

        for (guint i = 0; i < 9; i++)
            g_string_append_printf(entries, ", %s", entry);
        contents = mft_read(entries->str, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(contents);
        g_clear_error(&error);
    }

    /* too many files, which are not even looked at */
    {
        g_autofree char *entry = mft_entry_new(MAIN_ENTRY, MAIN_CONTENT, strlen(MAIN_CONTENT));
        g_autoptr(GString) entries = g_string_new(entry);

        for (guint i = 0; i < 1024; i++)
            g_string_append_printf(entries, ", %s", entry);
        contents = mft_read(entries->str, &error);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
        g_assert_null(contents);
    }
}

static void
test_tool_output_flood(void)
{
    g_autoptr(GHashTable) contents = NULL;
    g_autoptr(GError) error = NULL;

    /* the size limit stops a tool that never stops printing */
    contents = mft_read_with_fake_tool("if [ \"$1\" = --help ]; then echo --cat; exit 0; fi\nexec yes\n",
                                       &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
    g_assert_null(contents);
}

int
main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    mft_ensure_image();

    g_test_add_func("/image-reader/read-files", test_read_files);
    g_test_add_func("/image-reader/read-nothing", test_read_nothing);
    g_test_add_func("/image-reader/read-not-in-store", test_read_not_in_store);
    g_test_add_func("/image-reader/read-refused", test_read_refused);
    g_test_add_func("/image-reader/read-limits", test_read_limits);
    g_test_add_func("/image-reader/tool-output-flood", test_tool_output_flood);

    ret = g_test_run();
    g_clear_object(&store);
    mft_rm_rf(store_dir);

    return ret;
}
