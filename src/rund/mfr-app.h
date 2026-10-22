/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>
#include <systemd/sd-json.h>

G_BEGIN_DECLS

/**
 * MfrLayerKind:
 *
 * The layers of a mount tree, lowest first. Core, platform and kit are the
 * runtime of an application. Platform and kit are optional.
 */
typedef enum {
    MFR_LAYER_CORE,
    MFR_LAYER_PLATFORM,
    MFR_LAYER_KIT,
    MFR_LAYER_APP,
    /*< private >*/
    MFR_LAYER_LAST
} MfrLayerKind;

/**
 * MfrImage:
 *
 * The data needed to mount the filesystem of an image with dm-verity.
 */
typedef struct {
    char   *image_path;
    char   *verity_hash;
    guint64 data_offset; /* where the filesystem partition starts in the image */
    guint64 hash_offset; /* where the verity partition starts in the image */
} MfrImage;

/**
 * MfrLayer:
 *
 * One layer of a mount tree, with the component and the image of the layer.
 * If the application has no component for a layer, the verity hash of the
 * layer is %NULL.
 */
typedef struct {
    MfrLayerKind kind;
    char        *component_id;
    char        *version;
    MfrImage     image;
} MfrLayer;

#define MFR_TYPE_APP (mfr_app_get_type())
G_DECLARE_FINAL_TYPE(MfrApp, mfr_app, MFR, APP, GObject)

/**
 * MfrApp:
 *
 * An installed application, with the images of all its layers, as read
 * from the store. All of the fields here are read-only.
 */
struct _MfrApp {
    GObject  parent_instance;

    char    *component_id;
    char    *version;
    char    *command;    /* the program to run, below /app/bin */
    char    *arch_tuple; /* multiarch tuple of the application */
    MfrLayer layers[MFR_LAYER_LAST];
};

MfrApp *mfr_app_load(const char *component_id,
                     GError    **error);

int     mfr_app_append_layers_json(const MfrApp     *app,
                                   sd_json_variant **array);

G_END_DECLS
