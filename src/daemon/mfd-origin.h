/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include <manifold.h>

#include "mf-repo-private.h"

G_BEGIN_DECLS

/**
 * MfdOrigin:
 *
 * A channel of a remote that components can be installed from, with the
 * metadata of its last refresh.
 */
typedef struct {
    char                 *name; /* <remote>-<channel>, see mf_component_get_origin() */
    MfRemote             *remote;
    guint64               serial;           /* the serial of the root index */
    char                 *image_url_prefix; /* the URL prefix where images are looked up first, or NULL */
    MfRepoComponentIndex *components;
} MfdOrigin;

MfdOrigin *mfd_origin_ref(MfdOrigin *origin);
void       mfd_origin_unref(MfdOrigin *origin);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdOrigin, mfd_origin_unref)

/**
 * MfdOriginProblem:
 *
 * The reason why a remote, or one of its channels, can not be used.
 */
typedef struct {
    char   *remote_id;
    char   *channel; /* the channel that can not be used, or NULL if it is the whole remote */
    GError *error;
} MfdOriginProblem;

/**
 * MfdOriginSet:
 *
 * The origins of all remotes, as the last refresh stored them.
 */
typedef struct {
    GPtrArray *origins;  /* (element-type MfdOrigin): the origins we can install from */
    GPtrArray *problems; /* (element-type MfdOriginProblem): the remotes and channels we can not use */
} MfdOriginSet;

MfdOriginSet *mfd_origin_set_load(const char *root,
                                  GError    **error);
void          mfd_origin_set_free(MfdOriginSet *set);
MfdOrigin    *mfd_origin_set_find(MfdOriginSet *set,
                                  const char   *origin_name);
const GError *mfd_origin_set_find_problem(MfdOriginSet *set,
                                          const char   *origin_name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdOriginSet, mfd_origin_set_free)

MfRepoRootIndex *mfd_origin_read_root_index(const char    *root,
                                            MfRemote      *remote,
                                            MfTrustedKeys *keys,
                                            GBytes       **ret_data,
                                            GError       **error);
gboolean         mfd_origin_check_expiry(MfRepoRootIndex *index,
                                         GError         **error);

G_END_DECLS
