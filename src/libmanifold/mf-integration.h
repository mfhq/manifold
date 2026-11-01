/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <glib-object.h>

#include "mf-manifest.h"

G_BEGIN_DECLS

/**
 * MfIntegrationKind:
 * @MF_INTEGRATION_KIND_UNKNOWN: Not a known kind.
 * @MF_INTEGRATION_KIND_DESKTOP_ENTRY: An XDG desktop entry.
 * @MF_INTEGRATION_KIND_ICON: An icon of the hicolor theme.
 * @MF_INTEGRATION_KIND_MIME_PACKAGE: A shared-mime-info package.
 * @MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM: A systemd unit of the system manager.
 * @MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER: A systemd unit of the user managers.
 * @MF_INTEGRATION_KIND_UDEV_RULE: A udev rules file.
 *
 * The kinds of files an image can install on the host.
 */
typedef enum {
    MF_INTEGRATION_KIND_UNKNOWN,
    MF_INTEGRATION_KIND_DESKTOP_ENTRY,
    MF_INTEGRATION_KIND_ICON,
    MF_INTEGRATION_KIND_MIME_PACKAGE,
    MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM,
    MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER,
    MF_INTEGRATION_KIND_UDEV_RULE,
    /*< private >*/
    MF_INTEGRATION_KIND_LAST
} MfIntegrationKind;

const char       *mf_integration_kind_to_string(MfIntegrationKind kind);
MfIntegrationKind mf_integration_kind_from_string(const char *kind_str);

#define MF_TYPE_INTEGRATION_FILE (mf_integration_file_get_type())
G_DECLARE_FINAL_TYPE(MfIntegrationFile, mf_integration_file, MF, INTEGRATION_FILE, GObject)

MfIntegrationKind mf_integration_file_get_kind(MfIntegrationFile *file);
const char       *mf_integration_file_get_path(MfIntegrationFile *file);
const char       *mf_integration_file_get_checksum(MfIntegrationFile *file,
                                                   MfChecksumKind     kind);
gboolean          mf_integration_file_check_content(MfIntegrationFile *file,
                                                    GBytes            *content);
GBytes           *mf_integration_file_render(MfIntegrationFile *file,
                                             MfManifest        *manifest,
                                             GBytes            *content,
                                             GError           **error);

#define MF_TYPE_INTEGRATION_ALIAS (mf_integration_alias_get_type())
G_DECLARE_FINAL_TYPE(MfIntegrationAlias, mf_integration_alias, MF, INTEGRATION_ALIAS, GObject)

const char *mf_integration_alias_get_name(MfIntegrationAlias *alias);
const char *mf_integration_alias_get_command(MfIntegrationAlias *alias);

#define MF_TYPE_INTEGRATION (mf_integration_get_type())
G_DECLARE_FINAL_TYPE(MfIntegration, mf_integration, MF, INTEGRATION, GObject)

GPtrArray *mf_integration_get_files(MfIntegration *integration);
GPtrArray *mf_integration_get_aliases(MfIntegration *integration);
GPtrArray *mf_integration_get_unknown_kinds(MfIntegration *integration);

G_END_DECLS
