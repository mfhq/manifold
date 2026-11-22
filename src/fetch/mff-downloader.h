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
 * MffFailureKind:
 *
 * The reason why a request failed.
 */
typedef enum {
    MFF_FAILURE_KIND_NONE,
    MFF_FAILURE_KIND_NETWORK,
    MFF_FAILURE_KIND_HTTP,
    MFF_FAILURE_KIND_TOO_LARGE,
    MFF_FAILURE_KIND_BAD_RESPONSE,
    MFF_FAILURE_KIND_WRITE,
} MffFailureKind;

const char *mff_failure_kind_to_string(MffFailureKind failure);

typedef struct {
    uint64_t offset;
    uint64_t length;
} MffRange;

/**
 * MffRequest:
 *
 * A file or range of a file to download, as well as status information.
 */
typedef struct {
    char          *url;
    int            fd;       /* the file to write the data to, owned by the request */
    uint64_t       max_size; /* nothing is written at or after this offset */
    GArray        *ranges;   /* (element-type MffRange): the ranges to fetch, or NULL for the whole file */

    uint64_t       size; /* the number of bytes written */
    MffFailureKind failure;
    long           http_status; /* the HTTP status of the failed reply, or 0 */
    char          *message;
} MffRequest;

MffRequest *mff_request_new(const char *url,
                            int         fd,
                            uint64_t    max_size,
                            GArray     *ranges);
void        mff_request_free(MffRequest *request);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MffRequest, mff_request_free)

/**
 * MffProgressFunc:
 * @received_bytes: the number of bytes that were downloaded so far
 *
 * Called regularly while a download runs.
 *
 * Returns: %FALSE to stop the download
 */
typedef gboolean (*MffProgressFunc)(uint64_t received_bytes,
                                    gpointer user_data);

gboolean mff_download(GPtrArray      *requests,
                      const char     *ca_file,
                      MffProgressFunc progress_func,
                      gpointer        user_data,
                      GError        **error);

G_END_DECLS
