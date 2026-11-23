/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib-object.h>

#include <manifold.h>

#include "mfd-install-set.h"
#include "mfd-remotes.h"
#include "mfd-resolver.h"

G_BEGIN_DECLS

/**
 * MfdErrorDetail:
 * @component_id: the component that the error is about
 * @version: the version of @component_id, for %MF_ERROR_ALREADY_INSTALLED
 * @base_id: the missing base, for %MF_ERROR_MISSING_BASE
 * @base_version: the version of the missing base, for %MF_ERROR_MISSING_BASE
 * @identity: the identity that the missing base must provide, for %MF_ERROR_MISSING_BASE
 * @used_by: a component that needs @component_id, for %MF_ERROR_COMPONENT_IN_USE
 * @architecture: the architecture that @component_id was built for, for %MF_ERROR_WRONG_ARCHITECTURE
 * @kind: the kind of the installed component with the ID @component_id, for %MF_ERROR_ID_CONFLICT
 * @sdk: whether the installed component is an SDK, for %MF_ERROR_ID_CONFLICT
 *
 * Details about the failure of an operation, beyond the #GError.
 */
typedef struct {
    char           *component_id;
    char           *version;
    char           *base_id;
    char           *base_version;
    char           *identity;
    char           *used_by;
    char           *architecture;
    MfComponentKind kind;
    gboolean        sdk;
} MfdErrorDetail;

void mfd_error_detail_clear(MfdErrorDetail *detail);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfdErrorDetail,
                                 mfd_error_detail_clear)

/**
 * MfdRemovalItem:
 * @component: a version of a component that will be deleted
 * @action: the reason for the removal: %MF_ACTION_REMOVE if the removal was
 *   requested, %MF_ACTION_CLEANUP if nothing needs the version anymore
 * @superseded: %TRUE if the version is removed as part of an update of the component
 *
 * One version of a component that a removal deletes.
 */
typedef struct {
    MfComponent *component;
    MfAction     action;
    gboolean     superseded;
} MfdRemovalItem;

/**
 * MfdRemovalPlan:
 * @items: (element-type MfdRemovalItem): the versions that will be deleted, in removal order
 * @cpts_set: (element-type MfComponent): the components of @items, as a deduplication set
 * @images: (element-type utf8): the verity hashes of the images that will be removed
 * @image_bytes: the disk space that the images take up, in bytes
 *
 * The components and images that a removal will remove.
 */
typedef struct {
    GPtrArray  *items;
    GHashTable *cpts_set;
    GPtrArray  *images;
    uint64_t    image_bytes;
} MfdRemovalPlan;

void mfd_removal_plan_free(MfdRemovalPlan *rmplan);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRemovalPlan,
                              mfd_removal_plan_free)

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

MfdRemovalPlan      *mfd_manager_plan_removal(MfdManager     *manager,
                                              const char     *component_id,
                                              const char     *version,
                                              MfdErrorDetail *ret_detail,
                                              GError        **error);
MfResult            *mfd_manager_remove(MfdManager     *manager,
                                        const char     *component_id,
                                        const char     *version,
                                        MfProgressFunc  progress_func,
                                        gpointer        user_data,
                                        MfdErrorDetail *ret_detail,
                                        GError        **error);

MfdInstallSet       *mfd_manager_resolve(MfdManager     *manager,
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
