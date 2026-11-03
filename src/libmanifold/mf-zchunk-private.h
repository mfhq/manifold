/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

MF_INTERNAL_VISIBLE
GArray *mf_zchunk_find_image_chunks(const guint8 *data,
                                    gsize         size);

MF_END_PRIVATE_DECLS
