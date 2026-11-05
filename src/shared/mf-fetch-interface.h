/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "config.h"

#include <systemd/sd-varlink-idl.h>

#define MF_FETCH_METHOD_FETCH MF_FETCH_INTERFACE ".Fetch"

/* The highest number of requests in one call. Every request passes a file
 * descriptor, and a message can not carry more than 253 of them (SCM_MAX_FD). */
#define MF_FETCH_REQUESTS_MAX 253

extern const sd_varlink_interface vl_interface_org_freedesktop_manifold_fetch;
