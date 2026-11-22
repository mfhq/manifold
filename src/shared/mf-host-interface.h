/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "config.h"

#include <systemd/sd-varlink-idl.h>

/* methods */
#define MF_HOST_METHOD_RUN MF_HOST_INTERFACE ".Run"

/* errors */
#define MF_HOST_ERROR_NOT_PERMITTED_ID             MF_HOST_INTERFACE ".NotPermitted"
#define MF_HOST_ERROR_NO_SUCH_PROGRAM_ID           MF_HOST_INTERFACE ".NoSuchProgram"
#define MF_HOST_ERROR_NO_SUCH_WORKING_DIRECTORY_ID MF_HOST_INTERFACE ".NoSuchWorkingDirectory"
#define MF_HOST_ERROR_SPAWN_FAILED_ID              MF_HOST_INTERFACE ".SpawnFailed"

extern const sd_varlink_interface vl_interface_org_freedesktop_manifold_host;
