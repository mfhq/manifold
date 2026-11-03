/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <fcntl.h>
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

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/zchunk/chunks", test_chunks);
    g_test_add_func("/zchunk/write-image", test_write_image);

    return g_test_run();
}
