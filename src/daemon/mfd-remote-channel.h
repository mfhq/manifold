/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib.h>

#include <manifold.h>

#include "mf-repo-private.h"

G_BEGIN_DECLS

/**
 * MfdRemoteChannel:
 *
 * A channel of a remote that components can be installed from, with the
 * metadata of its last refresh.
 */
typedef struct {
    char                 *origin; /* <remote>-<channel>, see mf_component_get_origin() */
    MfRemote             *remote;
    uint64_t              serial;           /* the serial of the root index */
    char                 *image_url_prefix; /* the URL prefix where images are looked up first, or NULL */
    MfRepoComponentIndex *components;
} MfdRemoteChannel;

MfdRemoteChannel *mfd_remote_channel_ref(MfdRemoteChannel *channel);
void              mfd_remote_channel_unref(MfdRemoteChannel *channel);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRemoteChannel, mfd_remote_channel_unref)

/**
 * MfdRemoteProblem:
 *
 * The reason why a remote, or one of its channels, can not be used.
 */
typedef struct {
    char   *remote_id;
    char   *channel; /* the channel that can not be used, or NULL if it is the whole remote */
    GError *error;
} MfdRemoteProblem;

/**
 * MfdRemoteChannelSet:
 *
 * The channels of all remotes, as the last refresh stored them.
 */
typedef struct {
    GPtrArray *channels; /* (element-type MfdRemoteChannel): the channels we can install from */
    GPtrArray *problems; /* (element-type MfdRemoteProblem): the remotes and channels we can not use */
} MfdRemoteChannelSet;

MfdRemoteChannelSet *mfd_remote_channel_set_load(const char *root,
                                                 GError    **error);
void                 mfd_remote_channel_set_free(MfdRemoteChannelSet *set);
MfdRemoteChannel    *mfd_remote_channel_set_find(MfdRemoteChannelSet *set,
                                                 const char          *origin);
const GError        *mfd_remote_channel_set_find_problem(MfdRemoteChannelSet *set,
                                                         const char          *origin);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRemoteChannelSet, mfd_remote_channel_set_free)

MfRepoRootIndex *mfd_remote_read_cached_root_index(const char    *root,
                                                   MfRemote      *remote,
                                                   MfTrustedKeys *keys,
                                                   GBytes       **ret_data,
                                                   GError       **error);

G_END_DECLS
