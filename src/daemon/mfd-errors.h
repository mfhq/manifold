/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include <manifold.h>

#include "mf-error-private.h"
#include "mf-manifest-private.h"

G_BEGIN_DECLS

void mfd_set_error_already_installed(GError    **error,
                                     const char *component_id,
                                     const char *version);
void mfd_set_error_not_installed(GError    **error,
                                 const char *component_id,
                                 const char *version);
void mfd_set_error_in_use(GError    **error,
                          const char *component_id,
                          const char *used_by);
void mfd_set_error_missing_base(GError               **error,
                                const char            *component_id,
                                const MfBaseReference *ref);
void mfd_set_error_not_available(GError    **error,
                                 const char *component_id,
                                 const char *origin);
void mfd_set_error_base_not_available(GError               **error,
                                      const char            *component_id,
                                      const MfBaseReference *ref);
void mfd_set_error_id_conflict(GError     **error,
                               MfComponent *installed);
void mfd_set_error_wrong_architecture(GError    **error,
                                      const char *component_id,
                                      const char *architecture);

G_END_DECLS
