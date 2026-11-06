/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "mf-macros-private.h"
#include "mf-trusted-keys.h"

MF_BEGIN_PRIVATE_DECLS

MF_INTERNAL_VISIBLE
char *mf_public_key_normalize(const char *public_key);

MF_END_PRIVATE_DECLS
