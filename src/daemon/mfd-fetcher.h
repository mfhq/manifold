/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib.h>

G_BEGIN_DECLS

/**
 * MfdFetchRequest:
 *
 * A file or range of a file for the download helper to write into @fd, as well
 * as status information.
 */
typedef struct {
    char    *url;
    int      fd;       /* the file to write the data to, owned by the caller */
    uint64_t max_size; /* nothing is written at or after this offset */
    GArray  *ranges;   /* (element-type MfdFetchRange): the ranges to fetch, or NULL for the whole file */

    uint64_t size;        /* the number of bytes written */
    char    *failure;     /* the name of the failure in the fetch interface, or NULL */
    int64_t  http_status; /* the HTTP status of the failed reply, or 0 */
    char    *message;     /* the error message of the helper, or NULL */
} MfdFetchRequest;

typedef struct {
    uint64_t offset;
    uint64_t length;
} MfdFetchRange;

MfdFetchRequest *mfd_fetch_request_new(const char *url,
                                       int         fd,
                                       uint64_t    max_size);
void             mfd_fetch_request_free(MfdFetchRequest *request);
void             mfd_fetch_request_to_error(MfdFetchRequest *request,
                                            GError         **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdFetchRequest, mfd_fetch_request_free)

/**
 * MfdMemoryDownload:
 *
 * A small file to download into memory.
 */
typedef struct {
    char    *url;
    uint64_t max_size; /* the largest the file may be */
    GBytes  *data;     /* the content of the file, set once it was downloaded */
} MfdMemoryDownload;

void mfd_memory_download_clear(MfdMemoryDownload *download);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfdMemoryDownload,
                                 mfd_memory_download_clear)

typedef struct _MfdFetcher MfdFetcher;

typedef void (*MfdFetchProgressFunc)(uint64_t received,
                                     gpointer user_data);

MfdFetcher *mfd_fetcher_new(const char *helper_path,
                            const char *ca_file);
MfdFetcher *mfd_fetcher_ref(MfdFetcher *fetcher);
void        mfd_fetcher_unref(MfdFetcher *fetcher);
gboolean    mfd_fetcher_fetch(MfdFetcher          *fetcher,
                              GPtrArray           *requests,
                              MfdFetchProgressFunc progress_func,
                              gpointer             user_data,
                              GError             **error);
gboolean    mfd_fetcher_download_to_memory(MfdFetcher         *fetcher,
                                           MfdMemoryDownload **downloads,
                                           guint               n_downloads,
                                           GError            **error);
GBytes     *mfd_fetcher_download_file(MfdFetcher *fetcher,
                                      const char *url,
                                      uint64_t    max_size,
                                      GError    **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdFetcher, mfd_fetcher_unref)

G_END_DECLS
