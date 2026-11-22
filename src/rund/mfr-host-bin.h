/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>
#include <systemd/sd-event.h>

G_BEGIN_DECLS

/* directory on the host that contains the links to the programs of the host */
#define MFR_HOST_BIN_LINKS_DIR MF_RUN_CACHE_DIR "/host-bin"

#define MFR_TYPE_HOST_BIN (mfr_host_bin_get_type())
G_DECLARE_FINAL_TYPE(MfrHostBin, mfr_host_bin, MFR, HOST_BIN, GObject)

MfrHostBin        *mfr_host_bin_new(sd_event          *event,
                                    const char        *links_dir,
                                    const char *const *program_dirs,
                                    GError           **error);

gboolean           mfr_host_bin_sync(MfrHostBin *host_bin,
                                     GError    **error);

const char *const *mfr_host_bin_get_program_dirs(MfrHostBin *host_bin);

G_END_DECLS
