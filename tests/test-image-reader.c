/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>
#include <sys/stat.h>
#include <manifold.h>

#include "mf-store-private.h"
#include "mfd-image-reader.h"
#include "mft-utils.h"

/* the files of the image */
#define APP_DIR         "/app/share/applications/"
#define MAIN_ENTRY      APP_DIR "com.example.app.desktop"
#define LARGE_ENTRY     APP_DIR "com.example.app.large.desktop"     /* a file of 7 MiB */
#define TOO_LARGE_ENTRY APP_DIR "com.example.app.too-large.desktop" /* a file of 9 MiB */
#define DIR_ENTRY       APP_DIR "com.example.app.dir.desktop"       /* a directory */
#define LINK_ENTRY      APP_DIR "com.example.app.link.desktop"      /* a link to /etc/hostname */
#define MISSING_ENTRY   APP_DIR "com.example.app.missing.desktop"   /* not in the image */
#define MAIN_CONTENT    "[Desktop Entry]\nType=Application\nExec=app\n"

/* the directory of desktop entries, relative to the exports directory */
#define EXPORT_DIR "share/applications/"

/* the name of the image in the store */
#define VERITY_HASH "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/* the offset of the filesystem in the image, like in an image with partitions */
constexpr uint64_t DATA_OFFSET = 8192;

constexpr size_t LARGE_FILE_SIZE = 7 * 1024 * 1024;
constexpr size_t TOO_LARGE_FILE_SIZE = 9 * 1024 * 1024;

static char *store_dir = NULL;
static MfStore *store = NULL;

/**
 * mft_make_data:
 *
 * Make random data that can not be compressed, as dump.erofs takes a second
 * to decompress a large file of zeros. The data for a smaller size is the
 * start of the data for a larger size.
 *
 * Returns: (transfer full): @size bytes
 */
static char *
mft_make_data(size_t size)
{
    g_autoptr(GRand) rand = g_rand_new_with_seed(1);
    uint32_t *words = g_malloc(size + sizeof(uint32_t));

    for (size_t i = 0; i <= size / sizeof(uint32_t); i++)
        words[i] = g_rand_int(rand);

    return (char *) words;
}

/**
 * mft_write_sized_file:
 *
 * Write a file of the given size.
 */
static void
mft_write_sized_file(const char *path, size_t size)
{
    g_autofree char *content = mft_make_data(size);
    g_autoptr(GError) error = NULL;

    g_assert_true(g_file_set_contents(path, content, (ssize_t) size, &error));
    g_assert_no_error(error);
}

/**
 * mft_make_image_store:
 *
 * Make a store that holds a real image, made from the tree of an application,
 * with the filesystem at an offset.
 */
static void
mft_make_image_store(void)
{
    g_autofree char *mkfs = g_find_program_in_path("mkfs.erofs");
    g_autofree char *dump = g_find_program_in_path("dump.erofs");
    g_autofree char *tree = NULL;
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

    store_dir = mft_make_tmp_dir("image-reader");
    tree = g_build_filename(store_dir, "tree", NULL);
    main_entry = g_build_filename(tree, MAIN_ENTRY, NULL);
    large = g_build_filename(tree, LARGE_ENTRY, NULL);
    too_large = g_build_filename(tree, TOO_LARGE_ENTRY, NULL);
    directory = g_build_filename(tree, DIR_ENTRY, NULL);
    link = g_build_filename(tree, LINK_ENTRY, NULL);

    mft_make_dirs(directory);
    mft_write_file(main_entry, MAIN_CONTENT);
    mft_write_sized_file(large, LARGE_FILE_SIZE);
    mft_write_sized_file(too_large, TOO_LARGE_FILE_SIZE);
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
    g_assert_true(g_file_set_contents(image_path, image, (ssize_t) (DATA_OFFSET + size), &error));
    g_assert_no_error(error);

    store = mf_store_new_for_path(store_dir);
}

/**
 * mft_manifest_new_for_entries:
 * @entries: the entries of the desktop-entry list, as JSON
 * @verity_hash: the verity hash of the image that the manifest describes
 *
 * Returns: (transfer full): a manifest with the files of @entries
 */
static MfManifest *
mft_manifest_new_for_entries(const char *entries, const char *verity_hash)
{
    g_autofree char *section = g_strdup_printf("{\"desktop-entry\": [%s]}", entries);

    return mft_manifest_new(&(MftTestManifest){
        .component_id = "com.example.app",
        .kind = "app",
        .version = "1.0",
        .base_id = "net.example.core",
        .base_kind = "core",
        .integration = section,
        .verity_root_hash = verity_hash,
        .data_offset = DATA_OFFSET,
    });
}

/**
 * mft_entry_new:
 * @path: the path of the file in the image
 * @data: the content that the manifest entry records for the file
 *
 * Returns: (transfer full): an entry of the manifest
 */
static char *
mft_entry_new(const char *path, const void *data, size_t size)
{
    g_autofree char *sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, size);
    g_autofree char *blake3 = mft_blake3_hex(data, size);

    return g_strdup_printf("{\"path\": \"%s\", \"sha256\": \"%s\", \"blake3\": \"%s\"}",
                           path,
                           sha256,
                           blake3);
}

/**
 * mft_read:
 * @entries: the entries of the desktop-entry list, as JSON
 *
 * Returns: (transfer full) (nullable): the files that the reader returned, or %NULL on error
 */
static GHashTable *
mft_read(const char *entries, GError **error)
{
    g_autoptr(MfManifest) manifest = mft_manifest_new_for_entries(entries, VERITY_HASH);

    return mfd_image_read_integration(store, manifest, error);
}

/**
 * mft_assert_read_fails:
 * @entries: the entries of the desktop-entry list, as JSON
 *
 * Check that the files of a manifest can not be read from the image.
 */
static void
mft_assert_read_fails(const char *entries, MfError code)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GHashTable) contents = mft_read(entries, &error);

    g_assert_null(contents);
    mft_assert_error(error, code, NULL);
}

/**
 * mft_repeat_entry:
 *
 * Returns: (transfer full): a list of @count times the same entry of a manifest
 */
static char *
mft_repeat_entry(const char *entry, guint count)
{
    g_autoptr(GString) entries = g_string_new(entry);

    for (guint i = 1; i < count; i++)
        g_string_append_printf(entries, ", %s", entry);
    return g_string_free(g_steal_pointer(&entries), FALSE);
}

/**
 * mft_read_with_fake_tool:
 * @script: (nullable): a shell script to run as the tool, or %NULL to have none at all
 *
 * Read a file with a replacement for dump.erofs in the PATH.
 *
 * Returns: (transfer full) (nullable): the files that the reader returned, or %NULL on error
 */
static GHashTable *
mft_read_with_fake_tool(const char *script, GError **error)
{
    g_autofree char *dir = mft_make_tmp_dir("image-reader-tool");
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

/**
 * test_read_files:
 *
 * Read files of an image, and check them against the hashes of the manifest.
 */
static void
test_read_files(void)
{
    g_autofree char *data = mft_make_data(LARGE_FILE_SIZE);
    g_autofree char *main_entry = mft_entry_new(MAIN_ENTRY, MAIN_CONTENT, strlen(MAIN_CONTENT));
    g_autofree char *large = mft_entry_new(LARGE_ENTRY, data, LARGE_FILE_SIZE);
    g_autofree char *entries = g_strdup_printf("%s, %s", main_entry, large);
    g_autofree char *other_image = mft_fake_hash("other image");
    g_autoptr(MfManifest) without_files = mft_manifest_new_for_entries("", other_image);
    g_autoptr(MfManifest) not_in_store = mft_manifest_new_for_entries(main_entry, other_image);
    g_autoptr(GHashTable) contents = NULL;
    g_autoptr(GHashTable) nothing = NULL;
    g_autoptr(GError) error = NULL;
    GBytes *content;

    contents = mft_read(entries, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(g_hash_table_size(contents), ==, 2);

    content = g_hash_table_lookup(contents, EXPORT_DIR "com.example.app.desktop");
    g_assert_nonnull(content);
    g_assert_cmpmem(g_bytes_get_data(content, NULL),
                    g_bytes_get_size(content),
                    MAIN_CONTENT,
                    strlen(MAIN_CONTENT));
    content = g_hash_table_lookup(contents, EXPORT_DIR "com.example.app.large.desktop");
    g_assert_nonnull(content);
    g_assert_cmpuint(g_bytes_get_size(content), ==, LARGE_FILE_SIZE);

    /* a manifest without files does not need its image in the store */
    nothing = mfd_image_read_integration(store, without_files, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(g_hash_table_size(nothing), ==, 0);

    /* only images in the store are verified, so we must not read anything else */
    g_assert_null(mfd_image_read_integration(store, not_in_store, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
}

/**
 * test_read_refused:
 *
 * A file is refused if its content does not match the hashes of the manifest.
 */
static void
test_read_refused(void)
{
    g_autofree char *hostname = NULL;
    /* dump.erofs exits with 0 for all files in this table, so only the checksums detect the wrong content */
    struct {
        const char *path;
        const char *claimed;
    } files[] = {
        {MAIN_ENTRY,                "something else"}, /* different content */
        {MISSING_ENTRY,             ""              }, /* the tool prints nothing for a missing file */
        {DIR_ENTRY,                 ""              }, /* the tool prints nothing for a directory */
        {LINK_ENTRY,                NULL            }, /* a link is not followed to the host */
        {"/app/../../etc/hostname", NULL            }, /* a path does not lead out of the image */
    };

    /* claim the content of the host file for the paths that would reach it */
    if (!g_file_get_contents("/etc/hostname", &hostname, NULL, NULL))
        hostname = g_strdup("");

    for (size_t i = 0; i < G_N_ELEMENTS(files); i++) {
        const char *claimed = files[i].claimed != NULL ? files[i].claimed : hostname;
        g_autofree char *entry = mft_entry_new(files[i].path, claimed, strlen(claimed));

        g_test_message("%s", files[i].path);
        mft_assert_read_fails(entry, MF_ERROR_VERIFICATION_FAILED);
    }
}

/**
 * test_read_limits:
 *
 * A manifest can not make the daemon read more than 1024 files, a file
 * of more than 8 MiB, or more than 64 MiB altogether.
 */
static void
test_read_limits(void)
{
    g_autofree char *data = mft_make_data(TOO_LARGE_FILE_SIZE);
    g_autofree char *small = mft_entry_new(MAIN_ENTRY, MAIN_CONTENT, strlen(MAIN_CONTENT));
    g_autofree char *large = mft_entry_new(LARGE_ENTRY, data, LARGE_FILE_SIZE);
    g_autofree char *too_large = mft_entry_new(TOO_LARGE_ENTRY, data, TOO_LARGE_FILE_SIZE);
    g_autofree char *large_files = mft_repeat_entry(large, 10);
    g_autofree char *many_files = mft_repeat_entry(small, 1025);

    /* a single file that is too large */
    mft_assert_read_fails(too_large, MF_ERROR_INVALID_MANIFEST);

    /* 10 files of 7 MiB fit one by one, but not together */
    mft_assert_read_fails(large_files, MF_ERROR_INVALID_MANIFEST);

    /* too many files, which are refused before any file is read */
    mft_assert_read_fails(many_files, MF_ERROR_INVALID_MANIFEST);
}

/**
 * test_read_tool_fails:
 *
 * Files are read with dump.erofs, and the daemon has to cope with a tool
 * that is missing or misbehaves.
 */
static void
test_read_tool_fails(void)
{
    g_autoptr(GError) error = NULL;

    /* no tool at all */
    g_assert_null(mft_read_with_fake_tool(NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_FAILED);
    g_clear_error(&error);

    /* the size limit stops a tool that never stops printing */
    g_assert_null(
        mft_read_with_fake_tool("if [ \"$1\" = --help ]; then echo --cat; exit 0; fi\nexec yes\n", &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
}

int
main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    mft_make_image_store();

    g_test_add_func("/image-reader/read-files", test_read_files);
    g_test_add_func("/image-reader/read-refused", test_read_refused);
    g_test_add_func("/image-reader/read-limits", test_read_limits);
    g_test_add_func("/image-reader/read-tool-fails", test_read_tool_fails);

    ret = g_test_run();
    g_clear_object(&store);
    mft_rm_rf(store_dir);

    return ret;
}
