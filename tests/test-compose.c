/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <glib/gstdio.h>
#include <manifold.h>

#include "mfr-app.h"
#include "mf-store-private.h"
#include "mft-sandbox.h"
#include "mft-utils.h"

/**
 * mft_write_usual_store:
 *
 * Write the usual store: an application on a kit, on a platform, on a core.
 */
static void
mft_write_usual_store(void)
{
    mft_write_version(MF_STORAGE_DIR,
                      &(MftTestManifest){.component_id = TEST_CORE_ID, .kind = "core", .version = "13.4"},
                      NULL,
                      NULL);
    mft_write_version(MF_STORAGE_DIR,
                      &(MftTestManifest){.component_id = TEST_PLATFORM_ID,
                                         .kind = "platform",
                                         .version = "13.4",
                                         .base_id = TEST_CORE_ID,
                                         .base_kind = "core"},
                      NULL,
                      NULL);
    mft_write_version(MF_STORAGE_DIR,
                      &(MftTestManifest){.component_id = TEST_PLATFORM_KIT_ID,
                                         .kind = "kit",
                                         .version = "20260114T0238",
                                         .base_id = TEST_PLATFORM_ID,
                                         .base_kind = "platform"},
                      NULL,
                      NULL);
    mft_write_version(MF_STORAGE_DIR,
                      &(MftTestManifest){.component_id = TEST_APP_ID,
                                         .kind = "app",
                                         .version = "0.1.0",
                                         .base_id = TEST_PLATFORM_KIT_ID,
                                         .base_kind = "kit",
                                         .permissions = "[\"host-tmp\"]"},
                      NULL,
                      NULL);
}

/**
 * mft_assert_load_fails:
 * @message_part: (nullable): a part of the expected error message
 *
 * Check that an application can not be loaded.
 */
static void
mft_assert_load_fails(const char *component_id, MfError code, const char *message_part)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfrApp) app = mfr_app_load(component_id, &error);

    g_assert_null(app);
    mft_assert_error(error, code, message_part);
}

/**
 * mft_reset_store:
 *
 * Replace the contents of the store with the usual store of the tests.
 */
static void
mft_reset_store(void)
{
    mft_clear_store();
    mft_write_usual_store();
}

/**
 * test_load:
 *
 * Load an application with all of its layers.
 */
static void
test_load(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfrApp) app = NULL;
    g_autofree char *app_hash = mft_fake_hash(TEST_APP_ID);
    g_autofree char *core_hash = mft_fake_hash(TEST_CORE_ID);
    const MfrLayer *layer;

    mft_reset_store();

    app = mfr_app_load(TEST_APP_ID, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(app->component_id, ==, TEST_APP_ID);
    g_assert_cmpstr(app->version, ==, "0.1.0");
    g_assert_cmpstr(app->command, ==, "app");
    g_assert_cmpstr(app->arch_tuple, ==, "x86_64-linux-gnu");
    g_assert_true(mfr_app_has_permission(app, MF_PERMISSION_HOST_TMP));
    g_assert_false(mfr_app_has_permission(app, MF_PERMISSION_HOST_COMMANDS));

    layer = &app->layers[MFR_LAYER_APP];
    g_assert_cmpstr(layer->component_id, ==, TEST_APP_ID);
    g_assert_cmpstr(layer->image.verity_hash, ==, app_hash);
    g_assert_cmpuint(layer->image.data_offset, ==, 20480);
    g_assert_cmpuint(layer->image.hash_offset, ==, 22528);
    g_assert_cmpstr(app->layers[MFR_LAYER_KIT].component_id, ==, TEST_PLATFORM_KIT_ID);
    g_assert_cmpstr(app->layers[MFR_LAYER_KIT].version, ==, "20260114T0238");
    g_assert_cmpstr(app->layers[MFR_LAYER_PLATFORM].component_id, ==, TEST_PLATFORM_ID);
    g_assert_cmpstr(app->layers[MFR_LAYER_CORE].component_id, ==, TEST_CORE_ID);
    g_assert_cmpstr(app->layers[MFR_LAYER_CORE].image.verity_hash, ==, core_hash);
}

/**
 * test_without_platform:
 *
 * An application does not need a platform.
 */
static void
test_without_platform(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfrApp) app = NULL;

    mft_reset_store();

    /* a kit may sit directly on the core */
    mft_replace_link("../../../runtimes/" TEST_CORE_ID "/current",
                     MF_STORE_RUNTIMES_DIR "/" TEST_PLATFORM_KIT_ID "/20260114T0238-1/base");
    app = mfr_app_load(TEST_APP_ID, &error);

    g_assert_no_error(error);
    g_assert_null(app->layers[MFR_LAYER_PLATFORM].component_id);
    g_assert_cmpstr(app->layers[MFR_LAYER_CORE].component_id, ==, TEST_CORE_ID);
}

/**
 * test_load_refused:
 *
 * An application is not loaded if it is not installed, was revoked, or has no base.
 */
static void
test_load_refused(void)
{
    mft_reset_store();

    mft_assert_load_fails("org.example.nothing", MF_ERROR_NOT_FOUND, "org.example.nothing is not installed");
    /* a runtime is not an application */
    mft_assert_load_fails(TEST_CORE_ID, MF_ERROR_NOT_FOUND, NULL);

    /* a revoked application */
    mft_write_file(MF_STORE_APPS_DIR "/" TEST_APP_ID "/revoked", "");
    mft_assert_load_fails(TEST_APP_ID, MF_ERROR_REVOKED, TEST_APP_ID " was revoked");
    g_assert_cmpint(g_unlink(MF_STORE_APPS_DIR "/" TEST_APP_ID "/revoked"), ==, 0);

    /* a base that is not installed, and a missing `base` link */
    mft_replace_link("../../../runtimes/org.example.gone/current",
                     MF_STORE_APPS_DIR "/" TEST_APP_ID "/0.1.0-1/base");
    mft_assert_load_fails(TEST_APP_ID, MF_ERROR_MISSING_BASE, NULL);
    g_assert_cmpint(g_unlink(MF_STORE_APPS_DIR "/" TEST_APP_ID "/0.1.0-1/base"), ==, 0);
    mft_assert_load_fails(TEST_APP_ID, MF_ERROR_MISSING_BASE, TEST_APP_ID " 0.1.0 has no base");
}

/**
 * test_layers_must_be_in_order:
 *
 * In a broken store, a kit can be its own base. Loading the application has
 * to fail then, instead of following `base` links forever.
 */
static void
test_layers_must_be_in_order(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfrApp) app = NULL;

    mft_reset_store();

    /* make the kit its own base, so that a kit is below a kit */
    mft_replace_link("../../../runtimes/" TEST_PLATFORM_KIT_ID "/current",
                     MF_STORE_RUNTIMES_DIR "/" TEST_PLATFORM_KIT_ID "/20260114T0238-1/base");
    app = mfr_app_load(TEST_APP_ID, &error);
    g_assert_null(app);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "can not be below kit " TEST_PLATFORM_KIT_ID));
}

/**
 * test_sdk_is_not_supported_yet:
 *
 * An application can have an SDK as its base, but running such an
 * application is not supported yet.
 */
static void
test_sdk_is_not_supported_yet(void)
{
    mft_reset_store();

    mft_write_version(MF_STORAGE_DIR,
                      &(MftTestManifest){.component_id = TEST_PLATFORM_KIT_ID ".sdk",
                                         .kind = "kit",
                                         .version = "20260114T0238",
                                         .sdk = TRUE,
                                         .base_id = TEST_PLATFORM_KIT_ID,
                                         .base_kind = "kit",
                                         .base_sdk_id = TEST_PLATFORM_ID ".sdk",
                                         .base_sdk_kind = "platform"},
                      NULL,
                      NULL);
    mft_replace_link("../../../runtimes/" TEST_PLATFORM_KIT_ID ".sdk/current",
                     MF_STORE_APPS_DIR "/" TEST_APP_ID "/0.1.0-1/base");
    mft_assert_load_fails(TEST_APP_ID,
                          MF_ERROR_FAILED,
                          "running an application on an SDK is not supported yet");
}

int
main(int argc, char **argv)
{
    mft_sandbox_enter();
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/compose/load", test_load);
    g_test_add_func("/compose/without-platform", test_without_platform);
    g_test_add_func("/compose/load-refused", test_load_refused);
    g_test_add_func("/compose/layers-must-be-in-order", test_layers_must_be_in_order);
    g_test_add_func("/compose/sdk-is-not-supported-yet", test_sdk_is_not_supported_yet);

    return g_test_run();
}
