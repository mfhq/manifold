/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-zchunk.h"
#include "mf-zchunk-private.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>
#include <zck.h>
#include <zstd.h>

#include "mf-error.h"
#include "mf-manifest-private.h"
#include "mf-utils-private.h"

/*
 * Repositories serve images as zchunk files, so a client only downloads the
 * chunks it does not have in the images it installed already. That only works
 * well if the repository and the client split images at the same points, which
 * we implement here.
 */

/* images are split into blocks of the size the builder gives the filesystem */
constexpr gsize BLOCK_SIZE = 4096;

/* a chunk covers at most this many blocks (the cluster size the builder gives the filesystem) */
constexpr gsize CHUNK_BLOCKS_MAX = 16;

/* how far we look for the end of a zstd frame */
constexpr gsize FRAME_SIZE_MAX = 262144;

/* We treat a block as incompressible if its bytes are spread even-ish over all 256 values.
 * To measure that, we sum up the squared count of every byte value:
 * Two random bytes are equal (a collision) with a probability of sum / BLOCK_SIZE^2.
 * The limit is BLOCK_SIZE^2 / 2^7.5, so we require at least 7.5 of 8 bits of
 * (collision) entropy per byte. */
constexpr guint64 COLLISIONS_MAX = 92681;

/* how much we decompress at a time */
constexpr gsize READ_BUFFER_SIZE = 1024 * 1024;

constexpr guint8 ZSTD_FRAME_MAGIC[] = {0x28, 0xb5, 0x2f, 0xfd};

static void
mf_zck_free(zckCtx *zck)
{
    zck_free(&zck);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(zckCtx, mf_zck_free)

static void
mf_set_error_from_zck(GError **error, MfError code, zckCtx *zck, const char *message)
{
    g_autofree char *detail = g_strdup(zck != NULL ? zck_get_error(zck) : NULL);

    /* zchunk ends its messages with a newline */
    if (detail != NULL && g_strstrip(detail)[0] != '\0')
        g_set_error(error, MF_ERROR, code, "%s: %s", message, detail);
    else
        g_set_error_literal(error, MF_ERROR, code, message);
}

/**
 * mf_block_leading_zeros:
 *
 * Returns: the number of zero bytes a block starts with
 */
static gsize
mf_block_leading_zeros(const guint8 *block)
{
    gsize n = 0;

    while (n < BLOCK_SIZE && block[n] == 0)
        n++;
    return n;
}

/**
 * mf_block_is_incompressible:
 *
 * Check whether the bytes of a block are spread so evenly over all values that
 * compression would gain nothing.
 */
static gboolean
mf_block_is_incompressible(const guint8 *block)
{
    guint32 counts[256] = {};
    guint64 sum = 0;

    for (gsize i = 0; i < BLOCK_SIZE; i++)
        counts[block[i]]++;
    for (gsize i = 0; i < G_N_ELEMENTS(counts); i++)
        sum += (guint64) counts[i] * counts[i];

    return sum <= COLLISIONS_MAX;
}

/**
 * mf_image_frame_blocks:
 * @block: the index of a block of the image
 *
 * Find a zstd frame that starts in a block after its leading zero bytes, which
 * is how the filesystem stores a compressed cluster.
 *
 * Returns: the number of blocks the frame covers, starting at @block, or 0 if no frame starts there
 */
static gsize
mf_image_frame_blocks(const guint8 *data, gsize size, gsize block)
{
    gsize offset = block * BLOCK_SIZE;
    gsize zeros = mf_block_leading_zeros(data + offset);
    gsize start = offset + zeros;
    size_t frame_size;

    if (zeros == BLOCK_SIZE || start + sizeof(ZSTD_FRAME_MAGIC) > size ||
        memcmp(data + start, ZSTD_FRAME_MAGIC, sizeof(ZSTD_FRAME_MAGIC)) != 0)
        return 0;

    frame_size = ZSTD_findFrameCompressedSize(data + start, MIN(size - start, FRAME_SIZE_MAX));
    if (ZSTD_isError(frame_size))
        return 0;
    return (start + frame_size + BLOCK_SIZE - 1) / BLOCK_SIZE - block;
}

/**
 * mf_image_block_is_plain:
 *
 * Check whether a block holds data the filesystem stored without compression:
 * it is incompressible, and it neither is all zero nor starts a zstd frame.
 */
static gboolean
mf_image_block_is_plain(const guint8 *data, gsize size, gsize block)
{
    const guint8 *start = data + block * BLOCK_SIZE;

    return mf_block_leading_zeros(start) < BLOCK_SIZE && mf_image_frame_blocks(data, size, block) == 0 &&
           mf_block_is_incompressible(start);
}

/**
 * mf_chunks_add_blocks:
 *
 * Add chunks that cover @n_blocks blocks, as many as a chunk may cover at most.
 */
static void
mf_chunks_add_blocks(GArray *chunks, gsize n_blocks)
{
    while (n_blocks > 0) {
        gsize n = MIN(n_blocks, CHUNK_BLOCKS_MAX);
        guint64 chunk_size = n * BLOCK_SIZE;

        g_array_append_val(chunks, chunk_size);
        n_blocks -= n;
    }
}

/**
 * mf_zchunk_find_image_chunks:
 * @data: the image
 * @size: the size of the image
 *
 * Split an image into chunks. We cut at points that move along with the data
 * when a new version of the image shifts it by whole blocks, so the chunks that
 * did not change keep their content.
 *
 * Returns: (transfer full) (element-type guint64): the sizes of the chunks, in order
 */
GArray *
mf_zchunk_find_image_chunks(const guint8 *data, gsize size)
{
    GArray *chunks = g_array_new(FALSE, FALSE, sizeof(guint64));
    gsize n_blocks = size / BLOCK_SIZE;
    gsize b = 0;

    while (b < n_blocks) {
        const guint8 *block = data + b * BLOCK_SIZE;
        gsize frame_blocks;
        gsize n = 1;

        if (mf_block_leading_zeros(block) == BLOCK_SIZE) {
            /* a run of empty blocks */
            while (b + n < n_blocks && n < CHUNK_BLOCKS_MAX &&
                   mf_block_leading_zeros(data + (b + n) * BLOCK_SIZE) == BLOCK_SIZE)
                n++;
        } else if ((frame_blocks = mf_image_frame_blocks(data, size, b)) > 0) {
            /* a compressed cluster of the filesystem, which we never split at the end of the image */
            n = MIN(frame_blocks, n_blocks - b);
        } else if (mf_block_is_incompressible(block)) {
            /* data the filesystem stored without compression */
            while (b + n < n_blocks && n < CHUNK_BLOCKS_MAX && mf_image_block_is_plain(data, size, b + n))
                n++;
        }
        /* anything else, mostly filesystem metadata, stays a block of its own */

        mf_chunks_add_blocks(chunks, n);
        b += n;
    }

    /* images end in a GPT backup header, which is smaller than a block */
    if (size % BLOCK_SIZE != 0) {
        guint64 rest = size % BLOCK_SIZE;
        g_array_append_val(chunks, rest);
    }

    return chunks;
}

/**
 * mf_zchunk_write_data:
 *
 * Write an image that is in memory as a zchunk file.
 */
static gboolean
mf_zchunk_write_data(const guint8 *data, gsize size, int dest_fd, GError **error)
{
    g_autoptr(GArray) chunks = mf_zchunk_find_image_chunks(data, size);
    g_autoptr(zckCtx) zck = zck_create();
    gsize offset = 0;

    if (zck == NULL) {
        mf_set_error_from_zck(error, MF_ERROR_FAILED, NULL, _("Unable to create a zchunk file"));
        return FALSE;
    }
    /* the uncompressed digests let a client find the chunks it has in the images it installed */
    if (!zck_init_write(zck, dest_fd) || !zck_set_ioption(zck, ZCK_HASH_FULL_TYPE, ZCK_HASH_SHA256) ||
        !zck_set_ioption(zck, ZCK_HASH_CHUNK_TYPE, ZCK_HASH_SHA256) ||
        !zck_set_ioption(zck, ZCK_COMP_TYPE, ZCK_COMP_ZSTD) || !zck_set_ioption(zck, ZCK_MANUAL_CHUNK, 1) ||
        !zck_set_ioption(zck, ZCK_UNCOMP_HEADER, 1)) {
        mf_set_error_from_zck(error, MF_ERROR_FAILED, zck, _("Unable to create a zchunk file"));
        return FALSE;
    }

    for (guint i = 0; i < chunks->len; i++) {
        gsize chunk_size = g_array_index(chunks, guint64, i);

        if (zck_write(zck, (const char *) data + offset, chunk_size) != (ssize_t) chunk_size ||
            zck_end_chunk(zck) < 0) {
            mf_set_error_from_zck(error, MF_ERROR_FAILED, zck, _("Unable to write a zchunk file"));
            return FALSE;
        }
        offset += chunk_size;
    }

    if (!zck_close(zck)) {
        mf_set_error_from_zck(error, MF_ERROR_FAILED, zck, _("Unable to write a zchunk file"));
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_zchunk_write_image:
 * @image_fd: file descriptor of the image, opened for reading
 * @dest_fd: file descriptor to write the zchunk file to
 * @error: a #GError
 *
 * Write an image as a zchunk file, split into chunks the way repositories
 * split images. Chunks are compressed with zstd, and the header lists the
 * SHA-256 digests of the chunks, compressed and uncompressed.
 *
 * zchunk keeps the chunks in a temporary file until the zchunk file is
 * complete, which it creates in `$TMPDIR`.
 *
 * Returns: %TRUE if the zchunk file was written
 */
gboolean
mf_zchunk_write_image(int image_fd, int dest_fd, GError **error)
{
    struct stat st;
    guint8 *data = NULL;
    gboolean ret;

    g_return_val_if_fail(image_fd >= 0, FALSE);
    g_return_val_if_fail(dest_fd >= 0, FALSE);

    if (fstat(image_fd, &st) < 0) {
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("Unable to read the image: %s"), g_strerror(errno));
        return FALSE;
    }
    if (st.st_size > 0) {
        data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, image_fd, 0);
        if (data == MAP_FAILED) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_FAILED,
                        _("Unable to read the image: %s"),
                        g_strerror(errno));
            return FALSE;
        }
        madvise(data, st.st_size, MADV_SEQUENTIAL);
    }

    ret = mf_zchunk_write_data(data, st.st_size, dest_fd, error);

    if (data != NULL)
        munmap(data, st.st_size);
    return ret;
}

/**
 * mf_zchunk_open:
 *
 * Read the header of a zchunk file, from its start.
 *
 * Returns: (transfer full): the context of the zchunk file, or %NULL on error
 */
static zckCtx *
mf_zchunk_open(int fd, GError **error)
{
    g_autoptr(zckCtx) zck = NULL;

    if (lseek(fd, 0, SEEK_SET) < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Unable to read the zchunk file: %s"),
                    g_strerror(errno));
        return NULL;
    }
    zck = zck_create();
    if (zck == NULL || !zck_init_read(zck, fd)) {
        mf_set_error_from_zck(error, MF_ERROR_VERIFICATION_FAILED, zck, _("Not a valid zchunk file"));
        return NULL;
    }

    return g_steal_pointer(&zck);
}

/**
 * mf_zchunk_read_header:
 * @fd: file descriptor of a zchunk file, opened for reading
 * @header_size: (out): the number of bytes before the first chunk, the lead and the header together
 * @header_sha256: (out) (transfer full): the header checksum that zchunk records in the lead
 * @error: a #GError
 *
 * Read the header of a zchunk file. Its size and checksum are what a
 * repository index records about an image, so a client can check the header
 * before it parses it.
 *
 * Returns: %TRUE if the header could be read
 */
gboolean
mf_zchunk_read_header(int fd, guint64 *header_size, char **header_sha256, GError **error)
{
    g_autoptr(zckCtx) zck = NULL;
    char *digest;
    ssize_t length;

    g_return_val_if_fail(fd >= 0, FALSE);
    g_return_val_if_fail(header_size != NULL, FALSE);
    g_return_val_if_fail(header_sha256 != NULL, FALSE);

    zck = mf_zchunk_open(fd, error);
    if (zck == NULL)
        return FALSE;
    if (zck_get_full_hash_type(zck) != ZCK_HASH_SHA256) {
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_VERIFICATION_FAILED,
                            _("The header checksum of the zchunk file is not a SHA-256 checksum"));
        return FALSE;
    }

    length = zck_get_header_length(zck);
    digest = zck_get_header_digest(zck);
    if (length < 0 || digest == NULL) {
        free(digest);
        mf_set_error_from_zck(error, MF_ERROR_FAILED, zck, _("Unable to read the header of the zchunk file"));
        return FALSE;
    }

    *header_size = (guint64) length;
    *header_sha256 = g_strdup(digest);
    free(digest);
    return TRUE;
}

/**
 * mf_zchunk_check_image:
 * @fd: file descriptor of a zchunk file, opened for reading
 * @manifest: the manifest of the image the zchunk file is expected to hold
 * @error: a #GError
 *
 * Decompress a zchunk file, and check that it holds the image @manifest describes.
 * zchunk checks every chunk against its digest while we read it.
 *
 * Returns: %TRUE if the zchunk file holds the image
 */
gboolean
mf_zchunk_check_image(int fd, MfManifest *manifest, GError **error)
{
    g_autofree char *buffer = NULL;
    g_autoptr(zckCtx) zck = NULL;
    g_autoptr(MfHasher) hasher = NULL;
    MfFileEntry actual;
    guint64 image_size;
    guint64 total = 0;

    g_return_val_if_fail(fd >= 0, FALSE);
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), FALSE);

    zck = mf_zchunk_open(fd, error);
    if (zck == NULL)
        return FALSE;

    image_size = mf_manifest_get_image_size(manifest);
    buffer = g_malloc(READ_BUFFER_SIZE);
    hasher = mf_hasher_new();
    for (;;) {
        ssize_t n = zck_read(zck, buffer, READ_BUFFER_SIZE);

        if (n < 0) {
            mf_set_error_from_zck(error, MF_ERROR_VERIFICATION_FAILED, zck, _("The zchunk file is damaged"));
            return FALSE;
        }
        if (n == 0)
            break;

        /* an image that is too large can not match, so we read no further */
        total += (guint64) n;
        mf_hasher_update(hasher, buffer, (gsize) n);
        if (total > image_size)
            break;
    }

    mf_hasher_finish(hasher, &actual);
    if (!mf_manifest_image_matches(manifest, &actual)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("The zchunk file does not hold the image %s"),
                    mf_manifest_get_image_file(manifest));
        return FALSE;
    }

    return TRUE;
}
