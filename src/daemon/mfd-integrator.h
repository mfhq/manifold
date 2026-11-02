/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

#include <manifold.h>

#include "mfd-store-writer.h"

G_BEGIN_DECLS

#define MFD_TYPE_INTEGRATOR (mfd_integrator_get_type())
G_DECLARE_FINAL_TYPE(MfdIntegrator, mfd_integrator, MFD, INTEGRATOR, GObject)

MfdIntegrator *mfd_integrator_new(MfStore        *store,
                                  MfdStoreWriter *writer);

gboolean       mfd_integrator_check_conflicts(MfdIntegrator *integrator,
                                              GPtrArray     *manifests,
                                              GError       **error);

gboolean       mfd_integrator_reconcile(MfdIntegrator *integrator,
                                        MfResult      *result,
                                        const char    *component_id,
                                        GError       **error);
void           mfd_integrator_reconcile_all(MfdIntegrator *integrator);

G_END_DECLS
