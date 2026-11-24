/*
 * Copyright (C) 2020-2026 Collabora Ltd.
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean mfr_capture_build(const char *overrides_dir,
                           const char *core_mount,
                           const char *arch_tuple,
                           char     ***ret_environment,
                           GPtrArray **ret_host_dirs,
                           GError    **error);

G_END_DECLS
