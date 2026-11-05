/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <blake3.h>
#include <zck.h>
#include <zstd.h>
#include <manifold.h>

#include "mf-zchunk-private.h"
#include "mft-utils.h"

#define BS 4096

/**
 * mft_image_data:
 *
 * Make up an image with one region for every rule that splits an image into chunks:
 * 3 empty blocks, a metadata block, a zstd frame over 3 blocks, 18 incompressible
 * blocks, 20 empty blocks, another metadata block, and a short end like a GPT header.
 */
static GBytes *
mft_image_data(void)
{
    gsize size = (3 + 1 + 3 + 18 + 20 + 1) * BS + 512;
    g_autofree guint8 *data = g_malloc0(size);
    g_autofree guint8 *payload = g_malloc(10000);
    g_autofree guint8 *frame = NULL;
    g_autoptr(GRand) rand = g_rand_new_with_seed(42);
    gsize frame_bound = ZSTD_compressBound(10000);
    gsize frame_size;
    guint8 *pos;

    /* random data barely compresses, so its frame fills most of the 3 blocks */
    for (gsize i = 0; i < 10000; i++)
        payload[i] = (guint8) g_rand_int_range(rand, 0, 256);
    frame = g_malloc(frame_bound);
    frame_size = ZSTD_compress(frame, frame_bound, payload, 10000, 3);
    g_assert_false(ZSTD_isError(frame_size));
    g_assert_cmpuint(frame_size, >, 2 * BS);
    g_assert_cmpuint(frame_size, <=, 3 * BS);

    pos = data + 3 * BS;
    memset(pos, 'm', BS / 2);
    pos += BS;
    /* the frame ends with the third block, behind zero padding */
    memcpy(pos + 3 * BS - frame_size, frame, frame_size);
    pos += 3 * BS;
    for (gsize i = 0; i < 18 * BS; i++)
        pos[i] = (guint8) g_rand_int_range(rand, 0, 256);
    pos += (18 + 20) * BS;
    memset(pos, 'm', BS / 2);
    pos += BS;
    memset(pos, 'g', 512);

    return g_bytes_new_take(g_steal_pointer(&data), size);
}

static void
test_chunks(void)
{
    const guint64 expected[] = {3 * BS, BS, 3 * BS, 16 * BS, 2 * BS, 16 * BS, 4 * BS, BS, 512};
    g_autoptr(GBytes) image = mft_image_data();
    g_autoptr(GArray)
        chunks = mf_zchunk_find_image_chunks(g_bytes_get_data(image, NULL), g_bytes_get_size(image));

    g_assert_cmpmem(chunks->data, chunks->len * sizeof(guint64), expected, sizeof(expected));
}

/**
 * mft_manifest_for:
 *
 * Returns: (transfer full): a manifest that describes @image
 */
static MfManifest *
mft_manifest_for(GBytes *image, const char *sha256)
{
    g_autofree char *actual_sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, image);
    g_autofree char *blake3 = NULL;
    g_autofree char *json = NULL;
    g_autoptr(GBytes) json_bytes = NULL;
    g_autoptr(GError) error = NULL;
    guint8 hash[BLAKE3_OUT_LEN];
    blake3_hasher hasher;
    MfManifest *manifest;
    gsize json_size;

    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, g_bytes_get_data(image, NULL), g_bytes_get_size(image));
    blake3_hasher_finalize(&hasher, hash, sizeof(hash));
    blake3 = g_malloc(2 * BLAKE3_OUT_LEN + 1);
    for (gsize i = 0; i < BLAKE3_OUT_LEN; i++)
        g_snprintf(blake3 + 2 * i, 3, "%02x", hash[i]);

    json = mft_manifest_json(&(MftTestManifest){
        .component_id = TEST_CORE_ID,
        .kind = "core",
        .version = "1",
        .image_size = g_bytes_get_size(image),
        .sha256 = sha256 != NULL ? sha256 : actual_sha256,
        .blake3 = blake3,
    });
    json_size = strlen(json);
    json_bytes = g_bytes_new_take(g_steal_pointer(&json), json_size);
    manifest = mf_manifest_new_from_bytes(json_bytes, &error);
    g_assert_no_error(error);
    return manifest;
}

static void
test_write_image(void)
{
    g_autofree char *dir = mft_make_tmp_dir("zchunk");
    g_autofree char *image_path = g_build_filename(dir, "image.raw", NULL);
    g_autofree char *zck_path = g_build_filename(dir, "image.raw.zck", NULL);
    g_autoptr(GBytes) image = mft_image_data();
    g_autoptr(GArray)
        chunks = mf_zchunk_find_image_chunks(g_bytes_get_data(image, NULL), g_bytes_get_size(image));
    g_autoptr(MfManifest) manifest = mft_manifest_for(image, NULL);
    g_autofree char *other_sha256 = mft_fake_hash("other");
    g_autoptr(MfManifest) other_manifest = mft_manifest_for(image, other_sha256);
    g_autofree char *header_sha256 = NULL;
    g_autoptr(GError) error = NULL;
    guint64 header_size = 0;
    zckChunk *chunk;
    zckCtx *zck;
    guint n_chunks = 0;
    int image_fd;
    int zck_fd;

    g_assert_true(
        g_file_set_contents(image_path, g_bytes_get_data(image, NULL), g_bytes_get_size(image), NULL));
    image_fd = open(image_path, O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(image_fd, >=, 0);
    zck_fd = open(zck_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    g_assert_cmpint(zck_fd, >=, 0);

    g_assert_true(mf_zchunk_write_image(image_fd, zck_fd, &error));
    g_assert_no_error(error);

    /* the zchunk file has exactly the chunks of the rule, after the empty dictionary chunk */
    zck = zck_create();
    g_assert_cmpint(lseek(zck_fd, 0, SEEK_SET), ==, 0);
    g_assert_true(zck_init_read(zck, zck_fd));
    for (chunk = zck_get_first_chunk(zck); chunk != NULL; chunk = zck_get_next_chunk(chunk)) {
        if (zck_get_chunk_number(chunk) == 0)
            continue;
        g_assert_cmpuint(n_chunks, <, chunks->len);
        g_assert_cmpint(zck_get_chunk_size(chunk), ==, g_array_index(chunks, guint64, n_chunks));
        n_chunks++;
    }
    g_assert_cmpuint(n_chunks, ==, chunks->len);

    g_assert_true(mf_zchunk_read_header(zck_fd, &header_size, &header_sha256, &error));
    g_assert_no_error(error);
    g_assert_cmpint(header_size, ==, zck_get_header_length(zck));
    g_assert_cmpuint(strlen(header_sha256), ==, 64);
    zck_free(&zck);

    /* the zchunk file holds the image of its manifest, and no other */
    g_assert_true(mf_zchunk_check_image(zck_fd, manifest, &error));
    g_assert_no_error(error);
    g_assert_false(mf_zchunk_check_image(zck_fd, other_manifest, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);

    /* an image is not a zchunk file */
    g_assert_false(mf_zchunk_read_header(image_fd, &header_size, &header_sha256, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);

    close(image_fd);
    close(zck_fd);
    mft_rm_rf(dir);
}

/**
 * mft_open_with:
 *
 * Returns: a new file in @dir that holds @data, opened for reading and writing
 */
static int
mft_open_with(const char *dir, const char *name, GBytes *data)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);
    int fd;

    g_assert_true(g_file_set_contents(path, g_bytes_get_data(data, NULL), g_bytes_get_size(data), NULL));
    fd = open(path, O_RDWR | O_CLOEXEC);
    g_assert_cmpint(fd, >=, 0);
    return fd;
}

/**
 * mft_copy_ranges:
 *
 * Copy @ranges of the zchunk file at @zck_fd into @partial_fd, as a download would.
 */
static void
mft_copy_ranges(int zck_fd, int partial_fd, GArray *ranges, guint first)
{
    for (guint i = first; i < ranges->len; i++) {
        MfZchunkRange *range = &g_array_index(ranges, MfZchunkRange, i);
        g_autofree char *buffer = g_malloc(range->length);

        g_assert_cmpint(pread(zck_fd, buffer, range->length, range->offset), ==, range->length);
        g_assert_cmpint(pwrite(partial_fd, buffer, range->length, range->offset), ==, range->length);
    }
}

static void
test_assembly(void)
{
    g_autofree char *dir = mft_make_tmp_dir("zchunk-assembly");
    g_autoptr(GBytes) old_image = mft_image_data();
    g_autofree guint8 *new_data = g_memdup2(g_bytes_get_data(old_image, NULL), g_bytes_get_size(old_image));
    g_autoptr(GBytes) new_image = NULL;
    g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
    g_autoptr(MfManifest) manifest = NULL;
    g_autoptr(MfZchunkAssembly) assembly = NULL;
    g_autoptr(MfZchunkAssembly) refused = NULL;
    g_autoptr(GArray) ranges = NULL;
    g_autoptr(GArray) joined = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *header_sha256 = NULL;
    g_autofree char *wrong_sha256 = mft_fake_hash("header");
    g_autofree guint8 *written = NULL;
    guint64 header_size;
    guint64 downloaded = 0;
    gsize size = g_bytes_get_size(old_image);
    struct stat st;
    int old_fd;
    int new_fd;
    int zck_fd;
    int partial_fd;
    int dest_fd;

    /* a new version changes a metadata block and some data the filesystem stored without compression */
    new_data[3 * BS] = 'n';
    new_data[(3 + 1 + 3 + 2) * BS] ^= 0xff;
    new_image = g_bytes_new_take(g_steal_pointer(&new_data), size);
    manifest = mft_manifest_for(new_image, NULL);
    old_fd = mft_open_with(dir, "old.raw", old_image);
    new_fd = mft_open_with(dir, "new.raw", new_image);
    zck_fd = mft_open_with(dir, "new.raw.zck", empty);
    g_assert_true(mf_zchunk_write_image(new_fd, zck_fd, &error));
    g_assert_no_error(error);
    g_assert_true(mf_zchunk_read_header(zck_fd, &header_size, &header_sha256, &error));
    g_assert_no_error(error);

    /* we start with the header alone, which has to be the one the repository records */
    partial_fd = mft_open_with(dir, "partial.zck", empty);
    {
        g_autofree char *header = g_malloc(header_size);

        g_assert_cmpint(pread(zck_fd, header, header_size, 0), ==, header_size);
        g_assert_cmpint(pwrite(partial_fd, header, header_size, 0), ==, header_size);
    }
    refused = mf_zchunk_assembly_new(partial_fd, header_size, wrong_sha256, &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_assert_null(refused);
    g_clear_error(&error);
    assembly = mf_zchunk_assembly_new(partial_fd, header_size, header_sha256, &error);
    g_assert_no_error(error);

    /* the old version has most chunks, so we only download the two that changed */
    g_assert_true(mf_zchunk_assembly_add_source(assembly, old_fd, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(mf_zchunk_assembly_get_reused_size(assembly), ==, size - BS - 16 * BS);
    ranges = mf_zchunk_assembly_get_missing(assembly, 0);
    g_assert_cmpuint(ranges->len, ==, 2);
    for (guint i = 0; i < ranges->len; i++)
        downloaded += g_array_index(ranges, MfZchunkRange, i).length;
    g_assert_cmpint(fstat(zck_fd, &st), ==, 0);
    g_assert_cmpuint(downloaded, <, (guint64) st.st_size - header_size);
    /* ranges that are close enough become one */
    joined = mf_zchunk_assembly_get_missing(assembly, G_MAXUINT32);
    g_assert_cmpuint(joined->len, ==, 1);

    /* without the chunks we did not download, nothing is written */
    dest_fd = mft_open_with(dir, "dest.raw", empty);
    mft_copy_ranges(zck_fd, partial_fd, ranges, 1);
    g_assert_false(mf_zchunk_assembly_write_image(assembly, dest_fd, manifest, NULL, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);
    close(dest_fd);

    /* with them, the image is exactly the new version */
    g_clear_pointer(&assembly, mf_zchunk_assembly_free);
    mft_copy_ranges(zck_fd, partial_fd, ranges, 0);
    assembly = mf_zchunk_assembly_new(partial_fd, header_size, header_sha256, &error);
    g_assert_no_error(error);
    g_assert_true(mf_zchunk_assembly_add_source(assembly, old_fd, &error));
    g_assert_no_error(error);
    dest_fd = mft_open_with(dir, "dest.raw", empty);
    g_assert_true(mf_zchunk_assembly_write_image(assembly, dest_fd, manifest, NULL, NULL, &error));
    g_assert_no_error(error);
    written = g_malloc(size);
    g_assert_cmpint(pread(dest_fd, written, size, 0), ==, size);
    g_assert_cmpmem(written, size, g_bytes_get_data(new_image, NULL), size);

    close(old_fd);
    close(new_fd);
    close(zck_fd);
    close(partial_fd);
    close(dest_fd);
    mft_rm_rf(dir);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/zchunk/chunks", test_chunks);
    g_test_add_func("/zchunk/write-image", test_write_image);
    g_test_add_func("/zchunk/assembly", test_assembly);

    return g_test_run();
}
