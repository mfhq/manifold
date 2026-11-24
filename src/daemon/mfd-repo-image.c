/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include "config.h"
#include "mfd-repo-image.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>
#include <libglnx.h>

#include "mf-component-private.h"
#include "mf-manifest-private.h"
#include "mf-remote-private.h"
#include "mf-store-private.h"
#include "mf-zchunk-private.h"

/*
 * Write the image of a component that is installed from a remote. The image
 * is built from its zchunk file in the repository. We only download the chunks
 * that no installed image contains.
 */

/* the directory for partially downloaded zchunk files */
#define DOWNLOADS_DIR MF_CACHE_DIR "/downloads"

/* chunks with a gap smaller than this are downloaded as one range */
constexpr uint64_t RANGE_GAP_MAX = 32 * 1024;
/* the highest number of ranges we request for one image. If an image needs more, we allow larger gaps. */
constexpr guint RANGES_MAX = 4096;
/* the highest number of installed images that we search for reusable chunks */
constexpr guint CHUNK_SOURCES_MAX = 4;

/* the data needed to download the image of an install item from a remote */
typedef struct {
    MfStore *store;
    MfdFetcher *fetcher;
    MfdRemoteChannel *channel;
    MfRepoVersion *version; /* owned by the component index of @channel */
} MfdRepoSource;

static void
mfd_repo_source_free(MfdRepoSource *source)
{
    g_object_unref(source->store);
    mfd_fetcher_unref(source->fetcher);
    mfd_remote_channel_unref(source->channel);
    g_free(source);
}

/* state to convert the progress of a download into the progress of writing the image */
typedef struct {
    MfCopyProgressFunc func;
    gpointer user_data;
    uint64_t reused;     /* bytes of the image that are copied from installed images */
    uint64_t downloaded; /* bytes of the image that are downloaded */
    uint64_t compressed; /* the compressed size of the downloaded bytes */
} MfdDownloadProgress;

/**
 * mfd_on_download_progress:
 *
 * Report the progress of writing an image while it is downloaded. Reused
 * chunks are counted as written from the start, and downloaded chunks are
 * counted when they arrive.
 */
static void
mfd_on_download_progress(uint64_t received, gpointer user_data)
{
    MfdDownloadProgress *progress = user_data;
    double share = (double) MIN(received, progress->compressed) / (double) progress->compressed;

    progress->func(progress->reused + (uint64_t) (share * (double) progress->downloaded),
                   progress->user_data);
}

/**
 * mfd_fetch_ranges:
 * @ranges: (element-type MfZchunkRange): the ranges to download, ordered by offset
 * @progress_func: (nullable): called with the number of bytes received so far
 *
 * Download ranges of a file into @fd. Each range is written at its own offset.
 */
static gboolean
mfd_fetch_ranges(MfdFetcher *fetcher,
                 const char *url,
                 int fd,
                 GArray *ranges,
                 MfdFetchProgressFunc progress_func,
                 gpointer user_data,
                 GError **error)
{
    const MfZchunkRange *last = &g_array_index(ranges, MfZchunkRange, ranges->len - 1);
    g_autoptr(GPtrArray) requests = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_fetch_request_free);
    MfdFetchRequest *request = mfd_fetch_request_new(url, fd, last->offset + last->length);

    g_ptr_array_add(requests, request);
    request->ranges = g_array_sized_new(FALSE, FALSE, sizeof(MfdFetchRange), ranges->len);
    for (guint i = 0; i < ranges->len; i++) {
        const MfZchunkRange *range = &g_array_index(ranges, MfZchunkRange, i);
        MfdFetchRange fetch_range = {range->offset, range->length};

        g_array_append_val(request->ranges, fetch_range);
    }

    if (!mfd_fetcher_fetch(fetcher, requests, progress_func, user_data, error))
        return FALSE;
    if (request->failure != NULL) {
        mfd_fetch_request_set_error(request, error);
        return FALSE;
    }
    return TRUE;
}

/**
 * mfd_compare_newest_first:
 *
 * Compare two components, so the one with the newer version is sorted first.
 */
static int
mfd_compare_newest_first(gconstpointer a, gconstpointer b)
{
    MfComponent *component_a = (MfComponent *) a;
    MfComponent *component_b = (MfComponent *) b;

    return mf_version_compare_with_rev(mf_component_get_version(component_b),
                                       mf_component_get_revision(component_b),
                                       mf_component_get_version(component_a),
                                       mf_component_get_revision(component_a));
}

/**
 * mfd_is_other_kit_on_base:
 *
 * Returns: %TRUE if @component is a kit with a different ID than the kit of
 *   @manifest, and builds on the same base
 */
static gboolean
mfd_is_other_kit_on_base(MfComponent *component, MfManifest *manifest)
{
    g_autoptr(MfManifest) other = NULL;

    if (g_str_equal(mf_component_get_id(component), mf_manifest_get_component_id(manifest)) ||
        mf_component_is_sdk(component) != mf_manifest_is_sdk(manifest))
        return FALSE;
    other = mf_component_fetch_manifest(component, NULL);
    if (other == NULL)
        return FALSE;

    /* a kit always has a base */
    return g_str_equal(mf_manifest_get_base(other)->id, mf_manifest_get_base(manifest)->id);
}

/**
 * mfd_find_chunk_sources:
 *
 * Find the installed images that are most likely to share chunks with the
 * image of @manifest. These are the images of other versions of the same
 * component. A kit usually gets a new component ID when its identity changes,
 * so for a kit we also take the images of other kits on the same base. Newer
 * versions are listed first.
 *
 * Returns: (transfer full) (element-type utf8): the paths of the images
 */
static GPtrArray *
mfd_find_chunk_sources(MfStore *store, MfManifest *manifest)
{
    g_autoptr(GPtrArray) versions = mf_store_list_components(store,
                                                             mf_manifest_get_component_id(manifest),
                                                             MF_COMPONENT_KIND_UNKNOWN,
                                                             NULL);
    g_autoptr(GPtrArray) kits = NULL;
    g_autoptr(GPtrArray) candidates = g_ptr_array_new();
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

    if (versions != NULL) {
        g_ptr_array_sort_values(versions, mfd_compare_newest_first);
        g_ptr_array_extend(candidates, versions, NULL, NULL);
    }
    if (mf_manifest_get_kind(manifest) == MF_COMPONENT_KIND_KIT)
        kits = mf_store_list_components(store, NULL, MF_COMPONENT_KIND_KIT, NULL);
    if (kits != NULL) {
        g_ptr_array_sort_values(kits, mfd_compare_newest_first);
        for (guint i = 0; i < kits->len; i++) {
            MfComponent *kit = g_ptr_array_index(kits, i);

            if (mfd_is_other_kit_on_base(kit, manifest))
                g_ptr_array_add(candidates, kit);
        }
    }

    /* skip the image we are about to write, in case the store has it already */
    g_hash_table_add(seen, (gpointer) mf_manifest_get_verity_hash(manifest));
    for (guint i = 0; i < candidates->len && paths->len < CHUNK_SOURCES_MAX; i++) {
        MfComponent *component = g_ptr_array_index(candidates, i);
        char *path;

        if (!g_hash_table_add(seen, (gpointer) mf_component_get_verity_hash(component)))
            continue;
        path = mf_store_get_image_path(store, component);
        if (path != NULL)
            g_ptr_array_add(paths, path);
    }

    return paths;
}

/**
 * mfd_clear_fd:
 *
 * Close a file descriptor that is an element of a #GArray.
 */
static void
mfd_clear_fd(gpointer fd)
{
    g_clear_fd((int *) fd, NULL);
}

/**
 * mfd_write_image_from:
 * @url: the zchunk file of the image
 *
 * Write the image of a version from a zchunk file. We download the header of
 * the file first, which has the checksum of every chunk. Then we download the
 * chunks that no installed image contains. The downloaded data is stored in a
 * sparse temporary file, and the image is assembled from that file and the
 * installed images.
 */
static gboolean
mfd_write_image_from(MfdRepoSource *source,
                     MfManifest *manifest,
                     const char *url,
                     int fd,
                     MfCopyProgressFunc progress_func,
                     gpointer progress_data,
                     GError **error)
{
    MfRepoVersion *version = source->version;
    g_auto(GLnxTmpfile) partial = {0};
    g_autoptr(GArray) header = g_array_new(FALSE, FALSE, sizeof(MfZchunkRange));
    MfZchunkRange header_range = {0, version->zck_header_size};
    g_autoptr(MfZchunkAssembly) assembly = NULL;
    g_autoptr(GPtrArray) sources = NULL;
    g_autoptr(GArray) source_fds = g_array_new(FALSE, FALSE, sizeof(int));
    g_autoptr(GArray) missing = NULL;
    uint64_t gap_max = RANGE_GAP_MAX;

    g_array_set_clear_func(source_fds, mfd_clear_fd);
    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, DOWNLOADS_DIR, 0700, NULL, error) ||
        !glnx_open_anonymous_tmpfile_full(O_RDWR | O_CLOEXEC, DOWNLOADS_DIR, &partial, error))
        return FALSE;

    g_array_append_val(header, header_range);
    if (!mfd_fetch_ranges(source->fetcher, url, partial.fd, header, NULL, NULL, error))
        return FALSE;
    assembly = mf_zchunk_assembly_new(partial.fd,
                                      version->zck_header_size,
                                      version->zck_header_sha256,
                                      error);
    if (assembly == NULL)
        return FALSE;

    sources = mfd_find_chunk_sources(source->store, manifest);
    for (guint i = 0; i < sources->len; i++) {
        const char *path = g_ptr_array_index(sources, i);
        g_autoptr(GError) local_error = NULL;
        int source_fd = open(path, O_RDONLY | O_CLOEXEC);

        if (source_fd < 0) {
            g_warning("Unable to look for chunks in %s: %s", path, g_strerror(errno));
            continue;
        }
        g_array_append_val(source_fds, source_fd);
        if (!mf_zchunk_assembly_add_source(assembly, source_fd, &local_error))
            g_warning("Unable to look for chunks in %s: %s", path, local_error->message);
    }

    missing = mf_zchunk_assembly_get_missing(assembly, gap_max);
    while (missing->len > RANGES_MAX) {
        gap_max *= 4;
        g_array_unref(missing);
        missing = mf_zchunk_assembly_get_missing(assembly, gap_max);
    }
    if (missing->len > 0) {
        MfdDownloadProgress progress = {
            .func = progress_func,
            .user_data = progress_data,
            .reused = mf_zchunk_assembly_get_reused_size(assembly),
        };

        progress.downloaded = mf_manifest_get_image_size(manifest) - progress.reused;
        for (guint i = 0; i < missing->len; i++)
            progress.compressed += g_array_index(missing, MfZchunkRange, i).length;
        g_info("Downloading %" PRIu64 " bytes of %s, reusing %" PRIu64 " bytes of installed images",
               progress.compressed,
               url,
               progress.reused);
        if (!mfd_fetch_ranges(source->fetcher,
                              url,
                              partial.fd,
                              missing,
                              progress_func != NULL ? mfd_on_download_progress : NULL,
                              &progress,
                              error))
            return FALSE;
    }

    /* the download has reported the progress already, unless there was nothing to download */
    return mf_zchunk_assembly_write_image(assembly,
                                          fd,
                                          manifest,
                                          missing->len == 0 ? progress_func : NULL,
                                          progress_data,
                                          error);
}

/**
 * mfd_write_repo_image:
 *
 * Write the image of an install item that comes from a remote. If the channel
 * has an external location for images, we try that location first. We fall
 * back to the repository if the download fails or the image does not match.
 */
static gboolean
mfd_write_repo_image(MfdInstallItem *item,
                     int fd,
                     MfCopyProgressFunc progress_func,
                     gpointer progress_data,
                     GError **error)
{
    MfdRepoSource *source = item->source;
    const char *prefix = source->channel->image_url_prefix;
    g_autofree char *location = mf_repo_version_build_image_location(source->version);
    g_autofree char *url = mf_remote_build_url(source->channel->remote, location);

    if (prefix != NULL) {
        g_autofree char *external_url = g_strconcat(prefix,
                                                    "/",
                                                    mf_manifest_get_verity_hash(item->manifest),
                                                    MF_REPO_IMAGE_SUFFIX,
                                                    NULL);
        g_autoptr(GError) external_error = NULL;

        if (mfd_write_image_from(source,
                                 item->manifest,
                                 external_url,
                                 fd,
                                 progress_func,
                                 progress_data,
                                 &external_error))
            return TRUE;
        g_message("Unable to get the image of %s from %s, downloading it from the repository: %s",
                  item->name,
                  prefix,
                  external_error->message);
        if (ftruncate(fd, 0) < 0)
            return glnx_throw_errno_prefix(error, "Unable to truncate the image of %s", item->name);
    }

    return mfd_write_image_from(source, item->manifest, url, fd, progress_func, progress_data, error);
}

/**
 * mfd_repo_image_set_source:
 * @item: an item of an install set
 * @store: the store, which we search for reusable chunks
 * @fetcher: the fetcher to download with
 * @channel: the channel the component is installed from
 * @version: the version of the component, owned by the component index of @channel
 *
 * Set up an item of an install set, so its image is downloaded from a remote.
 */
void
mfd_repo_image_set_source(MfdInstallItem *item,
                          MfStore *store,
                          MfdFetcher *fetcher,
                          MfdRemoteChannel *channel,
                          MfRepoVersion *version)
{
    MfdRepoSource *source = g_new0(MfdRepoSource, 1);

    source->store = g_object_ref(store);
    source->fetcher = mfd_fetcher_ref(fetcher);
    source->channel = mfd_remote_channel_ref(channel);
    source->version = version;
    item->write_image = mfd_write_repo_image;
    item->source = source;
    item->source_free = (GDestroyNotify) mfd_repo_source_free;
}
