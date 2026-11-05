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

gboolean mfd_refresh_remote(const char *root,
                            MfdFetcher *fetcher,
                            MfRemote   *remote,
                            MfResult   *result,
                            gboolean   *ret_changed,
                            GError    **error);
gboolean mfd_refresh_forget_remote(const char *root,
                                   const char *remote_id,
                                   GError    **error);
gboolean mfd_refresh_remove_unconfigured(const char *root,
                                         GPtrArray  *remotes,
                                         GError    **error);

gboolean mfd_refresh_fetch_root_index(MfdFetcher *fetcher,
                                      const char *url,
                                      GBytes    **ret_data,
                                      GBytes    **ret_signature,
                                      GError    **error);

G_END_DECLS
