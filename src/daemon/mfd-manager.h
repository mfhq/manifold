/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib-object.h>

#include <manifold.h>

#include "mfd-errors.h"
#include "mfd-install-set.h"
#include "mfd-remotes.h"
#include "mfd-resolver.h"

G_BEGIN_DECLS

#define MFD_TYPE_MANAGER (mfd_manager_get_type())
G_DECLARE_FINAL_TYPE(MfdManager, mfd_manager, MFD, MANAGER, GObject)

MfdManager          *mfd_manager_new(void);
MfdManager          *mfd_manager_new_for_store(MfStore *store);

MfResult            *mfd_manager_install_set(MfdManager     *manager,
                                             MfdInstallSet  *set,
                                             MfProgressFunc  progress_func,
                                             gpointer        user_data,
                                             MfdErrorDetail *ret_detail,
                                             GError        **error);

gboolean             mfd_manager_install_set_gains_privileges(MfdManager    *manager,
                                                              MfdInstallSet *set);

MfResult            *mfd_manager_remove(MfdManager     *manager,
                                        const char     *component_id,
                                        const char     *version,
                                        MfProgressFunc  progress_func,
                                        gpointer        user_data,
                                        MfdErrorDetail *ret_detail,
                                        GError        **error);

MfdInstallSet       *mfd_manager_resolve_request(MfdManager     *manager,
                                                 MfdRepoRequest *request,
                                                 GError        **error);
gboolean             mfd_manager_refresh_for_retry(MfdManager     *manager,
                                                   MfdRepoRequest *request,
                                                   MfdInstallSet  *set,
                                                   const GError   *failure);

void                 mfd_manager_set_fetch_helper(MfdManager *manager,
                                                  const char *helper_path,
                                                  const char *ca_file);
MfResult            *mfd_manager_refresh(MfdManager        *manager,
                                         const char *const *remote_ids,
                                         MfProgressFunc     progress_func,
                                         gpointer           user_data,
                                         GError           **error);

MfdRemoteInspection *mfd_manager_inspect_remote(MfdManager *manager,
                                                const char *url,
                                                const char *public_key,
                                                GError    **error);
MfResult            *mfd_manager_add_remote(MfdManager        *manager,
                                            const char        *remote_id,
                                            const char        *url,
                                            const char        *public_key,
                                            const char *const *channels,
                                            MfProgressFunc     progress_func,
                                            gpointer           user_data,
                                            GError           **error);
gboolean             mfd_manager_remove_remote(MfdManager *manager,
                                               const char *remote_id,
                                               GError    **error);

GPtrArray           *mfd_manager_mark(MfdManager     *manager,
                                      const char     *component_id,
                                      const char     *version,
                                      MfInstallMode   mode,
                                      MfdErrorDetail *ret_detail,
                                      GError        **error);

G_END_DECLS
