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
#include "mfd-install-set.h"
#include "mfd-remote-channel.h"

G_BEGIN_DECLS

void mfd_repo_image_set_source(MfdInstallItem   *item,
                               MfStore          *store,
                               MfdFetcher       *fetcher,
                               MfdRemoteChannel *channel,
                               MfRepoVersion    *version);

G_END_DECLS
