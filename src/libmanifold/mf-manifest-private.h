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

/**
 * MfBaseReference:
 *
 * Reference to a component that an image was built on, as given in the
 * `base` or `base-sdk` section of a manifest.
 */
typedef struct {
    char           *id;
    MfComponentKind kind;
    char           *version;
    uint64_t        revision;
    char           *identity;
    char           *content_hash;
    gboolean        exact; /* the component is required in exactly this version and revision */
} MfBaseReference;

MF_INTERNAL_VISIBLE
const MfBaseReference *mf_manifest_get_base(MfManifest *manifest);
MF_INTERNAL_VISIBLE
const MfBaseReference *mf_manifest_get_base_sdk(MfManifest *manifest);
MF_INTERNAL_VISIBLE
gboolean mf_base_reference_matches_version(const MfBaseReference *ref,
                                           MfManifest            *manifest);

gboolean mf_manifest_image_matches(MfManifest        *manifest,
                                   const MfFileEntry *entry);

MF_END_PRIVATE_DECLS
