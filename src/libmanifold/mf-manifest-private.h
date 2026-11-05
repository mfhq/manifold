/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "mf-manifest.h"
#include "mf-macros-private.h"
#include "mf-utils-private.h"

MF_BEGIN_PRIVATE_DECLS

gboolean mf_manifest_image_matches(MfManifest        *manifest,
                                   const MfFileEntry *entry);

MF_END_PRIVATE_DECLS
