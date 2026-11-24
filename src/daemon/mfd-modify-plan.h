/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib.h>

#include <manifold.h>

#include "mf-store-private.h"

#include "mfd-errors.h"
#include "mfd-install-set.h"
#include "mfd-integrator.h"

G_BEGIN_DECLS

/**
 * MfdInstallPlan:
 *
 * The result of checking one item of an install set, which says how the item will be installed.
 */
typedef struct {
    MfdInstallItem *item; /* the item of the set, which outlives the plan */
    MfInstallMode   mode;
    gboolean        skip;      /* the item is installed already or is not needed, so there is nothing to do */
    gboolean        add_image; /* the image has to be added to the store */
    MfBaseLink     *base;      /* link to the base of the component, NULL for a core */
    /* link to the SDK of the layer below, NULL if the component has no base SDK */
    MfBaseLink     *base_sdk;
    /* the version of the same component that is installed now, if the item is newer than it.
     * NULL if the component is new, or if a newer version of it is installed already.*/
    MfComponent    *previous;
} MfdInstallPlan;

GArray *mfd_install_plans_new_for_set(MfStore       *store,
                                      MfdIntegrator *integrator,
                                      MfdInstallSet *set,
                                      GError       **error);

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

MfdRemovalPlan *mfd_removal_plan_new_for_removal(MfStore    *store,
                                                 const char *component_id,
                                                 const char *version,
                                                 GError    **error);
MfdRemovalPlan *mfd_removal_plan_new_for_superseded(MfStore *store,
                                                    GArray  *plans,
                                                    GError **error);

G_END_DECLS
