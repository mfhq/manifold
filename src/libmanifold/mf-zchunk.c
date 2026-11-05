/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
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

/**
 * MfZchunkAssembly:
 *
 * An image that is built from a partially downloaded zchunk file and from
 * images that are installed already. We split the installed images into
 * chunks with the same rule that repositories use, and look up the digest of
 * each chunk in the zchunk file. Only the chunks of the zchunk file that no
 * installed image already has will be downloaded.
 */
struct _MfZchunkAssembly {
    zckCtx *zck;
    GArray *chunks;     /* (element-type MfAssemblyChunk): the chunks of the image, in order */
    GHashTable *needed; /* digest of an uncompressed chunk -> index of the first chunk with that digest */
    GArray *source_fds; /* (element-type int): the images that chunks are copied from, owned by the caller */
    guint64 image_size;
};

typedef struct {
    zckChunk *chunk;
    guint64 start; /* the offset of the compressed chunk in the zchunk file */
    guint64 comp_size;
    guint64 size;
    guint64 offset; /* the offset of the chunk in the image */
    char digest[MF_HEX_HASH_LENGTH + 1];
    int source; /* the index of the image the chunk is copied from, or -1 if it is downloaded */
    guint64 source_offset;
} MfAssemblyChunk;

/**
 * mf_zchunk_assembly_free:
 */
void
mf_zchunk_assembly_free(MfZchunkAssembly *assembly)
{
    if (assembly == NULL)
        return;
    g_clear_pointer(&assembly->zck, mf_zck_free);
    g_clear_pointer(&assembly->chunks, g_array_unref);
    g_clear_pointer(&assembly->needed, g_hash_table_unref);
    g_clear_pointer(&assembly->source_fds, g_array_unref);
    g_free(assembly);
}

/**
 * mf_zchunk_assembly_new:
 * @partial_fd: the partially downloaded zchunk file, which must have its
 *   header. The file descriptor is owned by the caller.
 * @header_size: the size of the header that the repository records
 * @header_sha256: the checksum of the header that the repository records
 * @error: a #GError
 *
 * Read the header of a zchunk file. The size and checksum of the header are
 * verified before zchunk parses it.
 *
 * Returns: (transfer full): the assembly, or %NULL
 */
MfZchunkAssembly *
mf_zchunk_assembly_new(int partial_fd, guint64 header_size, const char *header_sha256, GError **error)
{
    g_autoptr(MfZchunkAssembly) assembly = g_new0(MfZchunkAssembly, 1);
    guint64 offset = 0;

    g_return_val_if_fail(partial_fd >= 0, NULL);
    g_return_val_if_fail(header_sha256 != NULL, NULL);

    assembly->chunks = g_array_new(FALSE, FALSE, sizeof(MfAssemblyChunk));
    assembly->needed = g_hash_table_new(g_str_hash, g_str_equal);
    assembly->source_fds = g_array_new(FALSE, FALSE, sizeof(int));

    if (lseek(partial_fd, 0, SEEK_SET) < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Unable to read the zchunk file: %s"),
                    g_strerror(errno));
        return NULL;
    }
    assembly->zck = zck_create();
    if (assembly->zck == NULL || !zck_init_adv_read(assembly->zck, partial_fd) ||
        !zck_set_ioption(assembly->zck, ZCK_VAL_HEADER_HASH_TYPE, ZCK_HASH_SHA256) ||
        !zck_set_soption(assembly->zck, ZCK_VAL_HEADER_DIGEST, header_sha256, strlen(header_sha256)) ||
        !zck_set_ioption(assembly->zck, ZCK_VAL_HEADER_LENGTH, (ssize_t) header_size) ||
        !zck_read_lead(assembly->zck) || !zck_read_header(assembly->zck)) {
        mf_set_error_from_zck(error,
                              MF_ERROR_VERIFICATION_FAILED,
                              assembly->zck,
                              _("The zchunk file does not have the expected header that matches repo metadata"));
        return NULL;
    }
    if (zck_get_chunk_hash_type(assembly->zck) != ZCK_HASH_SHA256) {
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_VERIFICATION_FAILED,
                            _("The chunks of the zchunk file do not have SHA-256 digests"));
        return NULL;
    }

    for (zckChunk *chunk = zck_get_first_chunk(assembly->zck); chunk != NULL;
         chunk = zck_get_next_chunk(chunk)) {
        MfAssemblyChunk item = {.chunk = chunk, .source = -1};
        char *digest;

        /* the first chunk is the compression dictionary, which our images do not use */
        if (zck_get_chunk_number(chunk) == 0 || zck_get_chunk_size(chunk) == 0)
            continue;

        digest = zck_get_chunk_digest_uncompressed(chunk);
        if (digest == NULL || strlen(digest) != MF_HEX_HASH_LENGTH) {
            free(digest);
            g_set_error_literal(error,
                                MF_ERROR,
                                MF_ERROR_VERIFICATION_FAILED,
                                _("The zchunk file has no digests of its uncompressed chunks"));
            return NULL;
        }
        g_strlcpy(item.digest, digest, sizeof(item.digest));
        free(digest);
        item.start = (guint64) zck_get_chunk_start(chunk);
        item.comp_size = (guint64) zck_get_chunk_comp_size(chunk);
        item.size = (guint64) zck_get_chunk_size(chunk);
        item.offset = offset;
        offset += item.size;
        g_array_append_val(assembly->chunks, item);
    }
    assembly->image_size = offset;

    /* NOTE: The keys point into the array, so we only fill the table once the array is complete */
    for (guint i = 0; i < assembly->chunks->len; i++) {
        MfAssemblyChunk *item = &g_array_index(assembly->chunks, MfAssemblyChunk, i);

        if (!g_hash_table_contains(assembly->needed, item->digest))
            g_hash_table_insert(assembly->needed, item->digest, GUINT_TO_POINTER(i));
    }

    return g_steal_pointer(&assembly);
}

/**
 * mf_zchunk_assembly_add_source:
 * @image_fd: an installed image. The file descriptor is owned by the caller,
 *   and has to stay open until the new image was written.
 *
 * Find the chunks of the zchunk file that an installed image contains, so
 * they do not have to be downloaded.
 *
 * Returns: %TRUE if the image could be read
 */
gboolean
mf_zchunk_assembly_add_source(MfZchunkAssembly *assembly, int image_fd, GError **error)
{
    g_autoptr(GArray) chunks = NULL;
    struct stat st;
    guint8 *data;
    guint64 offset = 0;
    int source = (int) assembly->source_fds->len;

    if (fstat(image_fd, &st) < 0) {
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("Unable to read the image: %s"), g_strerror(errno));
        return FALSE;
    }
    if (st.st_size == 0)
        return TRUE;
    data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, image_fd, 0);
    if (data == MAP_FAILED) {
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("Unable to read the image: %s"), g_strerror(errno));
        return FALSE;
    }
    madvise(data, st.st_size, MADV_SEQUENTIAL);

    chunks = mf_zchunk_find_image_chunks(data, st.st_size);
    for (guint i = 0; i < chunks->len; i++) {
        guint64 size = g_array_index(chunks, guint64, i);
        g_autofree char *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data + offset, size);
        gpointer index;

        if (g_hash_table_lookup_extended(assembly->needed, digest, NULL, &index)) {
            /* copy every chunk that has this digest from this position of the image */
            for (guint j = GPOINTER_TO_UINT(index); j < assembly->chunks->len; j++) {
                MfAssemblyChunk *item = &g_array_index(assembly->chunks, MfAssemblyChunk, j);

                if (item->source < 0 && item->size == size && g_str_equal(item->digest, digest)) {
                    item->source = source;
                    item->source_offset = offset;
                }
            }
            g_hash_table_remove(assembly->needed, digest);
        }
        offset += size;
    }
    munmap(data, st.st_size);

    g_array_append_val(assembly->source_fds, image_fd);
    return TRUE;
}

/**
 * mf_zchunk_assembly_get_missing:
 * @gap_max: ranges with a gap smaller than this are merged into one range
 *
 * Find the ranges of the zchunk file that have to be downloaded. These are
 * the chunks that no installed image contains. A range may include chunks
 * that we have already, if they are between two missing chunks, to keep the
 * number of requests low.
 *
 * Returns: (transfer full) (element-type MfZchunkRange): the ranges, ordered by offset
 */
GArray *
mf_zchunk_assembly_get_missing(MfZchunkAssembly *assembly, guint64 gap_max)
{
    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(MfZchunkRange));

    for (guint i = 0; i < assembly->chunks->len; i++) {
        MfAssemblyChunk *item = &g_array_index(assembly->chunks, MfAssemblyChunk, i);
        MfZchunkRange *last = ranges->len > 0 ? &g_array_index(ranges, MfZchunkRange, ranges->len - 1) : NULL;

        if (item->source >= 0 || item->comp_size == 0)
            continue;
        if (last != NULL && item->start <= last->offset + last->length + gap_max) {
            last->length = item->start + item->comp_size - last->offset;
        } else {
            MfZchunkRange range = {item->start, item->comp_size};

            g_array_append_val(ranges, range);
        }
    }

    return ranges;
}

/**
 * mf_zchunk_assembly_get_reused_size:
 *
 * Returns: the number of bytes of the image that are copied from installed images
 */
guint64
mf_zchunk_assembly_get_reused_size(MfZchunkAssembly *assembly)
{
    guint64 reused = 0;

    for (guint i = 0; i < assembly->chunks->len; i++) {
        MfAssemblyChunk *item = &g_array_index(assembly->chunks, MfAssemblyChunk, i);

        if (item->source >= 0)
            reused += item->size;
    }
    return reused;
}

/**
 * mf_copy_range:
 *
 * Copy a part of one file into another file. If the filesystem supports it,
 * the kernel shares the data blocks between the files instead of copying them.
 */
static gboolean
mf_copy_range(int src_fd, guint64 src_offset, int dest_fd, guint64 dest_offset, guint64 size, GError **error)
{
    loff_t in = (loff_t) src_offset;
    loff_t out = (loff_t) dest_offset;
    g_autofree char *buffer = NULL;

    while (size > 0) {
        ssize_t n = copy_file_range(src_fd, &in, dest_fd, &out, size, 0);

        if (n > 0) {
            size -= (guint64) n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || (errno != EXDEV && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP))
            break;

        /* the kernel can not copy between these files, so we copy the data ourselves */
        buffer = g_malloc(size);
        if (pread(src_fd, buffer, size, in) != (ssize_t) size ||
            pwrite(dest_fd, buffer, size, out) != (ssize_t) size)
            break;
        return TRUE;
    }
    if (size == 0)
        return TRUE;

    g_set_error(error,
                MF_ERROR,
                MF_ERROR_FAILED,
                _("Unable to copy a chunk of an image: %s"),
                g_strerror(errno));
    return FALSE;
}

/**
 * mf_zchunk_assembly_write_image:
 * @dest_fd: the file to write the image to, from its start
 * @manifest: the manifest of the image
 * @progress_func: (nullable) (scope call): called after every chunk, with the bytes written so far
 * @user_data: passed to @progress_func
 * @error: a #GError
 *
 * Write the image. Chunks that installed images contain are copied from
 * there. All other chunks are decompressed from the zchunk file, which must
 * have been downloaded by now. zchunk verifies the compressed digest of each
 * chunk it decompresses, and we verify the uncompressed digest. At the end we
 * verify the whole image against its manifest. If this function fails, the
 * data that was written is invalid and has to be discarded.
 *
 * Returns: %TRUE if the image was written and matches its manifest
 */
gboolean
mf_zchunk_assembly_write_image(MfZchunkAssembly *assembly,
                               int dest_fd,
                               MfManifest *manifest,
                               MfCopyProgressFunc progress_func,
                               gpointer user_data,
                               GError **error)
{
    g_autoptr(MfHasher) hasher = mf_hasher_new();
    g_autofree char *buffer = NULL;
    MfFileEntry actual;
    guint64 done = 0;

    g_return_val_if_fail(MF_IS_MANIFEST(manifest), FALSE);

    if (assembly->image_size != mf_manifest_get_image_size(manifest)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("The zchunk file does not hold the image %s"),
                    mf_manifest_get_image_file(manifest));
        return FALSE;
    }

    for (guint i = 0; i < assembly->chunks->len; i++) {
        MfAssemblyChunk *item = &g_array_index(assembly->chunks, MfAssemblyChunk, i);
        g_autofree char *digest = NULL;
        ssize_t n;

        if (item->source >= 0) {
            if (!mf_copy_range(g_array_index(assembly->source_fds, int, item->source),
                               item->source_offset,
                               dest_fd,
                               item->offset,
                               item->size,
                               error))
                return FALSE;
        } else {
            buffer = g_realloc(buffer, item->size);
            n = zck_get_chunk_data(item->chunk, buffer, item->size);
            digest = n == (ssize_t) item->size
                         ? g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *) buffer, item->size)
                         : NULL;
            if (g_strcmp0(digest, item->digest) != 0) {
                mf_set_error_from_zck(error,
                                      MF_ERROR_VERIFICATION_FAILED,
                                      assembly->zck,
                                      _("A chunk of the zchunk file is missing or damaged"));
                return FALSE;
            }
            if (pwrite(dest_fd, buffer, item->size, (off_t) item->offset) != (ssize_t) item->size) {
                g_set_error(error,
                            MF_ERROR,
                            MF_ERROR_FAILED,
                            _("Unable to write the image: %s"),
                            g_strerror(errno));
                return FALSE;
            }
        }

        done += item->size;
        if (progress_func != NULL)
            progress_func(done, user_data);
    }

    /* the copied chunks were only matched by their digest, so we verify the complete image */
    buffer = g_realloc(buffer, READ_BUFFER_SIZE);
    for (guint64 offset = 0; offset < assembly->image_size;) {
        ssize_t n = pread(dest_fd, buffer, READ_BUFFER_SIZE, (off_t) offset);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_FAILED,
                        _("Unable to read the image: %s"),
                        g_strerror(errno));
            return FALSE;
        }
        mf_hasher_update(hasher, buffer, (gsize) n);
        offset += (guint64) n;
    }
    mf_hasher_finish(hasher, &actual);
    if (!mf_manifest_image_matches(manifest, &actual)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("Image %s does not match what its manifest records"),
                    mf_manifest_get_image_file(manifest));
        return FALSE;
    }

    return TRUE;
}
