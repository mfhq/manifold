/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include <manifold.h>

#include "mfd-fetcher.h"
#include "mfd-install-set.h"
#include "mfd-remote-channel.h"

G_BEGIN_DECLS

/**
 * MfdRepoRequestKind:
 * @MFD_REPO_REQUEST_INSTALL: install one component
 * @MFD_REPO_REQUEST_UPDATE: update installed components
 *
 * The kind of a #MfdRepoRequest.
 */
typedef enum {
    MFD_REPO_REQUEST_INSTALL,
    MFD_REPO_REQUEST_UPDATE,
} MfdRepoRequestKind;

/**
 * MfdRepoRequest:
 *
 * A request to install or update components from the remotes.
 */
typedef struct {
    MfdRepoRequestKind kind;
    char              *component_id; /* the component to install, for an install request */
    char              *origin;       /* the origin to install the component from, or NULL */
    GStrv              update_ids;   /* the components to update, or NULL for all, for an update request */
    gboolean           refreshed; /* whether remotes were refreshed for the request, which we do only once */
} MfdRepoRequest;

MfdRepoRequest *mfd_repo_request_new_install(const char *component_id,
                                             const char *origin);
MfdRepoRequest *mfd_repo_request_new_update(const char *const *component_ids);
void            mfd_repo_request_free(MfdRepoRequest *request);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRepoRequest, mfd_repo_request_free)

gboolean mfd_resolver_resolve(MfdRepoRequest      *request,
                              MfdInstallSet       *set,
                              MfStore             *store,
                              MfdFetcher          *fetcher,
                              MfdRemoteChannelSet *channel_set,
                              GPtrArray           *failures,
                              GError             **error);

G_END_DECLS
