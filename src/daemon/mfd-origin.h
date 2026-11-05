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

MfRepoRootIndex *mfd_origin_read_root_index(const char    *root,
                                            MfRemote      *remote,
                                            MfTrustedKeys *keys,
                                            GBytes       **ret_data,
                                            GError       **error);
gboolean         mfd_origin_check_expiry(MfRepoRootIndex *index,
                                         GError         **error);

G_END_DECLS
