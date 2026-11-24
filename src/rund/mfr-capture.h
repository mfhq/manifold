/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define MFR_CAPTURE_DIR MF_RUN_CACHE_DIR "/capture"

#define MFR_TYPE_CAPTURE (mfr_capture_get_type())
G_DECLARE_FINAL_TYPE(MfrCapture, mfr_capture, MFR, CAPTURE, GObject)

const char        *mfr_capture_get_id(const MfrCapture *capture);
const char        *mfr_capture_get_overrides_dir(const MfrCapture *capture);
const char *const *mfr_capture_get_environment(const MfrCapture *capture);
gboolean           mfr_capture_is_fresh(const MfrCapture *capture);

#define MFR_TYPE_CAPTURES (mfr_captures_get_type())
G_DECLARE_FINAL_TYPE(MfrCaptures, mfr_captures, MFR, CAPTURES, GObject)

MfrCaptures *mfr_captures_new(void);
MfrCapture  *mfr_captures_acquire(MfrCaptures *captures,
                                  const char  *core_hash,
                                  const char  *arch_tuple,
                                  const char  *core_mount,
                                  GError     **error);
gboolean     mfr_captures_discard_all(MfrCaptures *captures,
                                      GError     **error);

void         mfr_captures_check_all(GHashTable *used_ids);

G_END_DECLS
