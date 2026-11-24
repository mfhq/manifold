/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include <manifold.h>

#include "mf-repo-private.h"
#include "mfd-fetcher.h"

G_BEGIN_DECLS

/**
 * MfdRemoteInspection:
 * @url: the URL of the repository, without a `/` at its end
 * @public_key: the public key that verified @index, without its comment
 * @index: the root index of the repository
 *
 * A repository that can be added as a remote. Its root index was downloaded
 * and verified with a public key.
 */
typedef struct {
    char            *url;
    char            *public_key;
    MfRepoRootIndex *index;
} MfdRemoteInspection;

void mfd_remote_inspection_free(MfdRemoteInspection *inspection);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRemoteInspection, mfd_remote_inspection_free)

MfdRemoteInspection *mfd_remotes_inspect(MfdFetcher *fetcher,
                                         const char *url,
                                         const char *public_key,
                                         const char *expected_id,
                                         GError    **error);
MfRemote            *mfd_remotes_add(MfdRemoteInspection *inspection,
                                     const char *const   *channels,
                                     GError             **error);
gboolean             mfd_remotes_remove(const char *root,
                                        const char *remote_id,
                                        GError    **error);

G_END_DECLS
