/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <stdint.h>
#include <glib-object.h>

#include "mf-component.h"

G_BEGIN_DECLS

typedef struct _MfUpdate MfUpdate;

#define MF_TYPE_UPDATE (mf_update_get_type())
GType     mf_update_get_type(void);

MfUpdate *mf_update_new(MfComponent *component,
                        const char  *previous_version);
MfUpdate *mf_update_copy(const MfUpdate *update);
void      mf_update_free(MfUpdate *update);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfUpdate,
                              mf_update_free)

MfComponent *mf_update_get_component(const MfUpdate *update);
const char  *mf_update_get_previous_version(const MfUpdate *update);
GPtrArray   *mf_update_get_integration_added(const MfUpdate *update);
GPtrArray   *mf_update_get_integration_removed(const MfUpdate *update);

void         mf_update_add_integration_added(MfUpdate   *update,
                                             const char *name);
void         mf_update_add_integration_removed(MfUpdate   *update,
                                               const char *name);

#define MF_TYPE_RESULT (mf_result_get_type())
G_DECLARE_DERIVABLE_TYPE(MfResult, mf_result, MF, RESULT, GObject)

struct _MfResultClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

MfResult  *mf_result_new(void);

GPtrArray *mf_result_get_updated(MfResult *result);
GPtrArray *mf_result_get_installed(MfResult *result);
GPtrArray *mf_result_get_removed(MfResult *result);
GPtrArray *mf_result_get_removed_images(MfResult *result);
uint64_t   mf_result_get_bytes_added(MfResult *result);
uint64_t   mf_result_get_bytes_freed(MfResult *result);
GPtrArray *mf_result_get_warnings(MfResult *result);

void       mf_result_add_updated(MfResult       *result,
                                 const MfUpdate *update);
void       mf_result_add_installed(MfResult    *result,
                                   MfComponent *component);
void       mf_result_add_removed(MfResult    *result,
                                 MfComponent *component);
void       mf_result_add_warning(MfResult   *result,
                                 const char *message);
void       mf_result_set_bytes_added(MfResult *result,
                                     uint64_t  bytes);
void       mf_result_set_bytes_freed(MfResult *result,
                                     uint64_t  bytes);

G_END_DECLS
