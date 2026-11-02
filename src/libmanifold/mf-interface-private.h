/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "config.h"

#include <systemd/sd-varlink-idl.h>

#include "mf-error.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

/*
 * The org.freedesktop.manifold Varlink interface, shared by the library that
 * calls it and the daemon that provides it.
 */

/* methods */
#define MF_VARLINK_METHOD_INSTALL_FILE     MF_INTERFACE ".InstallFile"
#define MF_VARLINK_METHOD_REMOVE_COMPONENT MF_INTERFACE ".RemoveComponent"
#define MF_VARLINK_METHOD_CLEANUP          MF_INTERFACE ".Cleanup"
#define MF_VARLINK_METHOD_MARK_COMPONENT   MF_INTERFACE ".MarkComponent"

/* transaction flags */
#define MF_VARLINK_TRANSACTION_FLAG_ALLOW_INTERACTIVE "allowInteractive"

/* exported so the daemon can serve it */
MF_INTERNAL_VISIBLE
extern const sd_varlink_interface vl_interface_org_freedesktop_manifold;

MF_INTERNAL_VISIBLE
const char *mf_error_to_varlink_id(MfError code);
gboolean    mf_error_from_varlink_id(const char *error_id,
                                     MfError    *ret_code);

MF_END_PRIVATE_DECLS
