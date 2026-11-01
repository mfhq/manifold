/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include <manifold.h>

G_BEGIN_DECLS

GHashTable *mfd_image_read_integration(MfStore    *store,
                                       MfManifest *manifest,
                                       GError    **error);

G_END_DECLS
