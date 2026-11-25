/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <zip.h>
#include <manifold.h>

#include "mf-manifest-private.h"

#include "mft-utils.h"

/* a made-up image for a bundle */
typedef struct TestImage TestImage;
struct TestImage {
    const char *component_id;
    const char *kind;
    const char *version;
    uint64_t revision;
    const TestImage *base; /* NULL for a core, or for a broken manifest */
    GBytes *data;
};

/* the flaws that a test can add to a bundle */
typedef struct {
    gboolean unsigned_;
    const char *unlisted_file; /* a file that the index does not list */
    const TestImage *corrupt;  /* an image whose content differs from its manifest */
    const TestImage *tampered; /* an image whose manifest differs from the index */
    const TestImage *missing;  /* an image that is not in the bundle */
    /* an image whose manifest records a wrong blake3, next to the right sha256 */
    const TestImage *wrong_blake3;
    /* an image whose manifest the index lists twice */
    const TestImage *listed_twice;
    /* an image that has no component in the catalog */
    const TestImage *undescribed;
    /* an image whose component in the catalog has a different ID */
    const TestImage *misdescribed;
    /* the icon of the app is not a JPEG XL image */
    gboolean bad_icon;
    /* the screenshot of the app does not match the hash that the index records */
    gboolean corrupt_media;
    /* a URL that the catalog uses for the screenshot of the app, instead of the screenshot file */
    const char *screenshot_url;
    /* an additional icon element for the app */
    const char *extra_icon;
} BundleFlaws;

/* the start of a JPEG XL codestream */
static const uint8_t JXL_DATA[] = {0xff, 0x0a, 'f', 'a', 'k', 'e'};

typedef struct {
    char *dir;
    TestImage core;
    TestImage kit;
    TestImage app;
} Fixture;

static GBytes *
mft_fake_image_data(const char *seed, size_t size)
{
    g_autofree uint8_t *data = g_malloc(size);

    for (size_t i = 0; i < size; i++)
        data[i] = (uint8_t) (seed[i % strlen(seed)] + i);
    return g_bytes_new_take(g_steal_pointer(&data), size);
}

static char *
mft_image_file_name(const TestImage *image)
{
    return g_strdup_printf("%s_%s.raw", image->component_id, image->version);
}

static char *
mft_image_manifest_json(const TestImage *image, gboolean wrong_blake3)
{
    g_autofree char *file_name = mft_image_file_name(image);
    g_autofree char *content_hash = mft_fake_hash(file_name);
    g_autofree char *sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, image->data);
    g_autofree char *blake3 = wrong_blake3 ? mft_fake_hash("wrong")
                                           : mft_blake3_hex(g_bytes_get_data(image->data, NULL),
                                                            g_bytes_get_size(image->data));
    g_autofree char *verity_hash = mft_fake_hash(sha256);
    const TestImage *base = image->base;

    return mft_manifest_json(&(MftTestManifest){
        .component_id = image->component_id,
        .kind = image->kind,
        .version = image->version,
        .revision = image->revision,
        .content_hash = content_hash,
        .base_id = base != NULL ? base->component_id : NULL,
        .base_kind = base != NULL ? base->kind : NULL,
        .base_version = base != NULL ? base->version : NULL,
        .base_revision = base != NULL ? base->revision : 0,
        .image_file = file_name,
        .image_size = g_bytes_get_size(image->data),
        .sha256 = sha256,
        .blake3 = blake3,
        .verity_root_hash = verity_hash,
        .hash_offset = 24576,
    });
}

static void
mft_bundle_add(zip_t *zip, const char *name, gconstpointer data, size_t size)
{
    zip_source_t *source = zip_source_buffer(zip, g_memdup2(data, size), size, 1);

    g_assert_nonnull(source);
    g_assert_cmpint(zip_file_add(zip, name, source, ZIP_FL_ENC_UTF_8), >=, 0);
}

/**
 * mft_index_new:
 *
 * Returns: (transfer full): the start of the index of a bundle, which lists no file yet
 */
static GString *
mft_index_new(void)
{
    return g_string_new("{\"format\": 1,\n\"files\": [");
}

/**
 * mft_index_add:
 *
 * Add the entry for a file to the index of a bundle.
 */
static void
mft_index_add(GString *index, const char *name, gconstpointer data, size_t size)
{
    g_autofree char *sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, size);
    g_autofree char *blake3 = mft_blake3_hex(data, size);

    /* clang-format off */
    g_string_append_printf(
        index,
        "%s\n"
        "      {\n"
        "         \"name\": \"%s\",\n"
        "         \"size\": %zu,\n"
        "         \"sha256\": \"%s\",\n"
        "         \"blake3\": \"%s\"\n"
        "      }",
        index->str[index->len - 1] == '[' ? "" : ",",
        name,
        size,
        sha256,
        blake3);
    /* clang-format on */
}

/**
 * mft_add_file:
 *
 * Add a file to a bundle and to its index.
 */
static void
mft_add_file(zip_t *zip, GString *index, const char *name, gconstpointer data, size_t size)
{
    mft_index_add(index, name, data, size);
    mft_bundle_add(zip, name, data, size);
}

/**
 * mft_bundle_add_index:
 *
 * Finish the index of a bundle, and add it to the bundle.
 */
static void
mft_bundle_add_index(zip_t *zip, GString *index)
{
    g_string_append(index, "\n   ]\n}");
    mft_bundle_add(zip, "index.json", index->str, index->len);
}

/**
 * mft_write_catalog:
 *
 * Add the catalog of the images to a bundle, with an icon and a screenshot
 * for the app, and the given flaws.
 */
static void
mft_write_catalog(zip_t *zip, GString *index, const TestImage *const *images, const BundleFlaws *flaws)
{
    g_autoptr(GString) components = g_string_new(NULL);
    g_autofree char *catalog = NULL;

    for (size_t i = 0; images[i] != NULL; i++) {
        const TestImage *image = images[i];
        g_autofree char *ref = g_strdup_printf("x86_64/%s/%s", image->component_id, image->version);
        g_autofree char *extra = NULL;
        g_autofree char *component = NULL;

        if (image == flaws->undescribed)
            continue;
        if (g_str_equal(image->kind, "app")) {
            g_autofree char *icon_name = g_strdup_printf("catalog/icons/64x64/%s.jxl", image->component_id);
            g_autofree char *media_name = g_strdup_printf("catalog/media/%s/screenshot.jxl",
                                                          image->component_id);
            g_autofree char *screenshot_url = g_strdup_printf("%s/screenshot.jxl", image->component_id);

            extra = g_strdup_printf(
                "<icon type=\"cached\" width=\"64\" height=\"64\">%s.jxl</icon>"
                "<icon type=\"stock\">%s</icon>%s"
                "<screenshots><screenshot type=\"default\">"
                "<image type=\"source\">%s</image>"
                "</screenshot></screenshots>",
                image->component_id,
                image->component_id,
                flaws->extra_icon != NULL ? flaws->extra_icon : "",
                flaws->screenshot_url != NULL ? flaws->screenshot_url : screenshot_url);
            if (flaws->bad_icon)
                mft_add_file(zip, index, icon_name, "not an image", strlen("not an image"));
            else
                mft_add_file(zip, index, icon_name, JXL_DATA, sizeof(JXL_DATA));

            mft_index_add(index, media_name, JXL_DATA, sizeof(JXL_DATA));
            if (flaws->corrupt_media)
                mft_bundle_add(zip, media_name, "changed", strlen("changed"));
            else
                mft_bundle_add(zip, media_name, JXL_DATA, sizeof(JXL_DATA));
        }
        component = mft_catalog_component_xml(image == flaws->misdescribed ? "org.example.Other"
                                                                           : image->component_id,
                                              ref,
                                              extra);
        g_string_append(components, component);
    }

    catalog = mft_catalog_xml(components->str);
    mft_add_file(zip, index, "catalog/manifold-local.xml", catalog, strlen(catalog));
}

/**
 * mft_write_bundle:
 *
 * Write a bundle of the images, with the given flaws. A bundle that
 * the test wrote before is replaced.
 *
 * Returns: (transfer full): the path of the bundle
 */
static char *
mft_write_bundle(Fixture *f, const TestImage *const *images, const BundleFlaws *flaws)
{
    g_autofree char *path = g_build_filename(f->dir, "test.manifold", NULL);
    g_autoptr(GString) index = mft_index_new();
    zip_t *zip = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, NULL);

    g_assert_nonnull(zip);
    for (size_t i = 0; images[i] != NULL; i++) {
        const TestImage *image = images[i];
        g_autofree char *file_name = mft_image_file_name(image);
        g_autofree char *base_name = g_strndup(file_name, strlen(file_name) - strlen(".raw"));
        g_autofree char *manifest_name = g_strconcat(base_name, ".manifold.json", NULL);
        g_autofree char *manifest = mft_image_manifest_json(image, image == flaws->wrong_blake3);
        gsize data_size;
        const uint8_t *data = g_bytes_get_data(image->data, &data_size);

        mft_index_add(index, manifest_name, manifest, strlen(manifest));
        if (image == flaws->listed_twice)
            mft_index_add(index, manifest_name, manifest, strlen(manifest));

        if (image == flaws->tampered) {
            g_autofree char *changed = g_strconcat(manifest, "\n", NULL);

            mft_bundle_add(zip, manifest_name, changed, strlen(changed));
        } else {
            mft_bundle_add(zip, manifest_name, manifest, strlen(manifest));
        }

        if (image == flaws->corrupt) {
            g_autofree uint8_t *changed = g_memdup2(data, data_size);

            changed[0] ^= 0xff;
            mft_bundle_add(zip, file_name, changed, data_size);
        } else if (image != flaws->missing) {
            mft_bundle_add(zip, file_name, data, data_size);
        }
    }
    mft_write_catalog(zip, index, images, flaws);
    mft_bundle_add_index(zip, index);

    if (!flaws->unsigned_)
        mft_bundle_add(zip, "index.json.sig", "not a signature", strlen("not a signature"));
    if (flaws->unlisted_file != NULL)
        mft_bundle_add(zip, flaws->unlisted_file, "rm -rf /", strlen("rm -rf /"));

    g_assert_cmpint(zip_close(zip), ==, 0);
    return g_steal_pointer(&path);
}

/**
 * mft_write_app_bundle:
 *
 * Write a bundle of the kit and the application, with the given flaws.
 *
 * Returns: (transfer full): the path of the bundle
 */
static char *
mft_write_app_bundle(Fixture *f, const BundleFlaws *flaws)
{
    return mft_write_bundle(f, (const TestImage *[]){&f->kit, &f->app, NULL}, flaws);
}

/**
 * mft_open_bundle:
 *
 * Returns: (transfer full): the bundle at @path, which has to be valid
 */
static MfBundle *
mft_open_bundle(const char *path)
{
    g_autofd int fd = open(path, O_RDONLY | O_CLOEXEC);
    g_autoptr(GError) error = NULL;
    MfBundle *bundle;

    g_assert_cmpint(fd, >=, 0);
    bundle = mf_bundle_new_from_fd(fd, &error);
    g_assert_no_error(error);
    return bundle;
}

static MfBundleImage *
mft_find_image(MfBundle *bundle, const char *component_id)
{
    GPtrArray *images = mf_bundle_get_images(bundle);

    for (guint i = 0; i < images->len; i++) {
        MfBundleImage *image = g_ptr_array_index(images, i);

        if (g_strcmp0(mf_manifest_get_component_id(mf_bundle_image_get_manifest(image)), component_id) == 0)
            return image;
    }
    g_assert_not_reached();
}

static void
mft_setup(Fixture *f, gconstpointer user_data)
{
    f->dir = mft_make_tmp_dir("bundle");

    f->core = (TestImage){TEST_CORE_ID, "core", "13.4", 22910000, NULL, mft_fake_image_data("core", 8192)};
    f->kit = (TestImage){TEST_KIT_ID,
                         "kit",
                         "20260112T0238",
                         22910051,
                         &f->core,
                         mft_fake_image_data("kit", 4096)};
    f->app = (TestImage){TEST_APP_ID, "app", "0.1.0", 22931700, &f->kit, mft_fake_image_data("app", 12288)};
}

static void
mft_teardown(Fixture *f, gconstpointer user_data)
{
    mft_rm_rf(f->dir);
    g_free(f->dir);
    g_bytes_unref(f->core.data);
    g_bytes_unref(f->kit.data);
    g_bytes_unref(f->app.data);
}

/* the progress reported while an image was copied */
typedef struct {
    uint64_t bytes_copied;
    guint n_reports;
} CopyProgress;

static void
mft_count_progress(uint64_t bytes_copied, gpointer user_data)
{
    CopyProgress *progress = user_data;

    g_assert_cmpuint(bytes_copied, >, progress->bytes_copied);
    progress->bytes_copied = bytes_copied;
    progress->n_reports++;
}

/**
 * test_read:
 *
 * Read a bundle with a core, a kit and an application.
 */
static void
test_read(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             (const TestImage *[]){&f->core, &f->kit, &f->app, NULL},
                                             &(BundleFlaws){});
    g_autoptr(MfBundle) bundle = mft_open_bundle(path);
    g_autofree char *kit_identity = mft_fake_hash(TEST_KIT_ID);
    g_autofree char *media_copy = g_build_filename(f->dir, "screenshot.jxl", NULL);
    g_autofree char *media_data = NULL;
    g_auto(GStrv) media = NULL;
    g_autoptr(GError) error = NULL;
    MfBundleImage *app = mft_find_image(bundle, TEST_APP_ID);
    MfBundleImage *kit = mft_find_image(bundle, TEST_KIT_ID);
    MfBundleImage *core = mft_find_image(bundle, TEST_CORE_ID);
    const char *app_catalog = g_bytes_get_data(mf_bundle_image_get_catalog(app), NULL);
    const char *kit_catalog = g_bytes_get_data(mf_bundle_image_get_catalog(kit), NULL);
    gsize media_size;
    GHashTable *icons;
    GBytes *icon;
    g_autofd int fd = -1;

    g_assert_true(mf_bundle_is_signed(bundle));
    g_assert_cmpuint(mf_bundle_get_images(bundle)->len, ==, 3);

    /* the manifests of the images */
    g_assert_cmpstr(mf_bundle_image_get_manifest_name(app), ==, TEST_APP_ID "_0.1.0.manifold.json");
    g_assert_cmpstr(mf_manifest_get_version(mf_bundle_image_get_manifest(app)), ==, "0.1.0");
    g_assert_cmpstr(mf_manifest_get_base(mf_bundle_image_get_manifest(app))->id, ==, TEST_KIT_ID);
    g_assert_cmpstr(mf_manifest_get_base(mf_bundle_image_get_manifest(kit))->id, ==, TEST_CORE_ID);
    g_assert_null(mf_manifest_get_base(mf_bundle_image_get_manifest(core)));

    /* the app's base is found in the bundle by its identity */
    g_assert_true(mf_bundle_find_image_for_identity(bundle, kit_identity) == kit);
    g_assert_null(mf_bundle_find_image_for_identity(bundle, "no-such-identity"));

    /* every image has its own catalog data, the app also its icon */
    g_assert_nonnull(strstr(app_catalog, "<id>" TEST_APP_ID "</id>"));
    g_assert_nonnull(strstr(app_catalog, "origin=\"manifold-local\""));
    g_assert_null(strstr(app_catalog, TEST_KIT_ID));
    g_assert_nonnull(strstr(kit_catalog, "<id>" TEST_KIT_ID "</id>"));
    icons = mf_bundle_image_get_icons(app);
    g_assert_cmpuint(g_hash_table_size(icons), ==, 1);
    icon = g_hash_table_lookup(icons, "64x64/" TEST_APP_ID ".jxl");
    g_assert_nonnull(icon);
    g_assert_cmpmem(g_bytes_get_data(icon, NULL), g_bytes_get_size(icon), JXL_DATA, sizeof(JXL_DATA));
    g_assert_cmpuint(g_hash_table_size(mf_bundle_image_get_icons(kit)), ==, 0);

    /* media files are named the way the catalog refers to them, and verified while they are copied */
    media = mf_bundle_list_media_files(bundle);
    g_assert_cmpuint(g_strv_length(media), ==, 1);
    g_assert_cmpstr(media[0], ==, TEST_APP_ID "/screenshot.jxl");
    fd = open(media_copy, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    g_assert_cmpint(fd, >=, 0);
    g_assert_true(mf_bundle_copy_media_file(bundle, media[0], fd, &error));
    g_assert_no_error(error);
    g_assert_false(mf_bundle_copy_media_file(bundle, "missing.jxl", fd, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_true(g_file_get_contents(media_copy, &media_data, &media_size, NULL));
    g_assert_cmpmem(media_data, media_size, JXL_DATA, sizeof(JXL_DATA));
}

/**
 * test_copy_image:
 *
 * An image is copied in pieces of 1 MiB, with progress after every piece.
 */
static void
test_copy_image(Fixture *f, gconstpointer user_data)
{
    /* the image takes three pieces, and the last piece is not full */
    size_t image_size = 2 * 1024 * 1024 + 12288;
    g_autofree char *path = NULL;
    g_autofree char *copy_path = g_build_filename(f->dir, "copy.raw", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = NULL;
    g_autofree char *copied = NULL;
    gsize copied_size;
    CopyProgress progress = {};
    g_autofd int fd = -1;

    g_bytes_unref(f->app.data);
    f->app.data = mft_fake_image_data("app", image_size);
    path = mft_write_app_bundle(f, &(BundleFlaws){});
    bundle = mft_open_bundle(path);

    fd = open(copy_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    g_assert_cmpint(fd, >=, 0);
    g_assert_true(mf_bundle_copy_image(bundle,
                                       mft_find_image(bundle, TEST_APP_ID),
                                       fd,
                                       mft_count_progress,
                                       &progress,
                                       &error));
    g_assert_no_error(error);

    g_assert_cmpuint(progress.n_reports, ==, 3);
    g_assert_cmpuint(progress.bytes_copied, ==, image_size);
    g_assert_true(g_file_get_contents(copy_path, &copied, &copied_size, NULL));
    g_assert_cmpmem(copied, copied_size, g_bytes_get_data(f->app.data, NULL), image_size);
}

/**
 * mft_assert_only_kit_verifies:
 *
 * Check that the image of the application fails its verification, and
 * that the image of the kit passes.
 */
static void
mft_assert_only_kit_verifies(MfBundle *bundle)
{
    g_autoptr(GError) error = NULL;

    g_assert_false(mf_bundle_verify_image(bundle, mft_find_image(bundle, TEST_APP_ID), NULL, NULL, &error));
    mft_assert_error(error,
                     MF_ERROR_VERIFICATION_FAILED,
                     "does not match its size and hashes in the manifest");
    g_clear_error(&error);
    g_assert_true(mf_bundle_verify_image(bundle, mft_find_image(bundle, TEST_KIT_ID), NULL, NULL, &error));
    g_assert_no_error(error);
}

/**
 * test_verify_image:
 *
 * Opening a bundle does not verify its images. An image is verified when
 * it is copied out of the bundle, or on request.
 */
static void
test_verify_image(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_app_bundle(f, &(BundleFlaws){.corrupt = &f->app});
    g_autoptr(MfBundle) bundle = mft_open_bundle(path);
    g_autoptr(GError) error = NULL;
    g_autofd int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

    /* an image with changed content */
    g_assert_false(mf_bundle_copy_image(bundle, mft_find_image(bundle, TEST_APP_ID), fd, NULL, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    mft_assert_only_kit_verifies(bundle);

    /* both hashes are checked, so a wrong blake3 hash next to the right sha256 hash fails */
    g_clear_object(&bundle);
    g_free(path);
    path = mft_write_app_bundle(f, &(BundleFlaws){.wrong_blake3 = &f->app});
    bundle = mft_open_bundle(path);
    mft_assert_only_kit_verifies(bundle);
}

/**
 * mft_assert_open_fails:
 * @message_part: a part of the expected error message
 *
 * Check that a file is refused when it is opened as a bundle.
 */
static void
mft_assert_open_fails(const char *path, MfError code, const char *message_part)
{
    g_autofd int fd = open(path, O_RDONLY | O_CLOEXEC);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = NULL;

    g_assert_cmpint(fd, >=, 0);
    bundle = mf_bundle_new_from_fd(fd, &error);
    g_assert_null(bundle);
    mft_assert_error(error, code, message_part);
}

/**
 * mft_assert_refused:
 * @message_part: a part of the expected error message
 *
 * Check that the bundle of the kit and the application is refused if it has the given flaws.
 */
static void
mft_assert_refused(Fixture *f, const BundleFlaws *flaws, MfError code, const char *message_part)
{
    g_autofree char *path = mft_write_app_bundle(f, flaws);

    mft_assert_open_fails(path, code, message_part);
}

/**
 * test_refused_files:
 *
 * A bundle is refused if its files do not match its index, or if it
 * contains files that the index does not list.
 */
static void
test_refused_files(Fixture *f, gconstpointer user_data)
{
    TestImage kit_without_base = f->kit;
    TestImage app_with_kit_id = f->app;
    g_autofree char *path = NULL;

    /* an image that is not in the bundle */
    mft_assert_refused(f,
                       &(BundleFlaws){.missing = &f->kit},
                       MF_ERROR_VERIFICATION_FAILED,
                       TEST_KIT_ID "_20260112T0238.raw, which is not in the bundle");

    /* a manifest that the index lists twice */
    mft_assert_refused(f,
                       &(BundleFlaws){.listed_twice = &f->app},
                       MF_ERROR_INVALID_BUNDLE,
                       TEST_APP_ID "_0.1.0.manifold.json, but a file can only be referenced once");

    /* a manifest and a screenshot that differ from the index */
    mft_assert_refused(f,
                       &(BundleFlaws){.tampered = &f->app},
                       MF_ERROR_VERIFICATION_FAILED,
                       TEST_APP_ID "_0.1.0.manifold.json does not match its size and hashes in the index");
    mft_assert_refused(f,
                       &(BundleFlaws){.corrupt_media = TRUE},
                       MF_ERROR_VERIFICATION_FAILED,
                       "screenshot.jxl does not match its size and hashes in the index");

    /* a file that the index does not list */
    mft_assert_refused(f,
                       &(BundleFlaws){.unlisted_file = "evil.sh"},
                       MF_ERROR_INVALID_BUNDLE,
                       "evil.sh, which its index does not list");

    /* the catalog directory is the only directory of a bundle, and its files have plain names */
    mft_assert_refused(f,
                       &(BundleFlaws){.unlisted_file = "catalog/../evil.sh"},
                       MF_ERROR_INVALID_BUNDLE,
                       "a file with an invalid name");
    mft_assert_refused(f,
                       &(BundleFlaws){.unlisted_file = "catalog/.hidden"},
                       MF_ERROR_INVALID_BUNDLE,
                       "a file with an invalid name");
    mft_assert_refused(f,
                       &(BundleFlaws){.unlisted_file = "catalog//x"},
                       MF_ERROR_INVALID_BUNDLE,
                       "a file with an invalid name");
    mft_assert_refused(f,
                       &(BundleFlaws){.unlisted_file = "lib/evil.sh"},
                       MF_ERROR_INVALID_BUNDLE,
                       "a file with an invalid name");

    /* an invalid manifest is reported with the name of its file */
    kit_without_base.base = NULL;
    path = mft_write_bundle(f, (const TestImage *[]){&kit_without_base, NULL}, &(BundleFlaws){});
    mft_assert_open_fails(path,
                          MF_ERROR_INVALID_MANIFEST,
                          TEST_KIT_ID
                          "_20260112T0238.manifold.json: a kit must name the layer it is built on");

    /* the same component ID as two kinds of component */
    g_free(path);
    app_with_kit_id.component_id = TEST_KIT_ID;
    path = mft_write_bundle(f, (const TestImage *[]){&f->kit, &app_with_kit_id, NULL}, &(BundleFlaws){});
    mft_assert_open_fails(path, MF_ERROR_INVALID_BUNDLE, TEST_KIT_ID " as two different kinds of component");
}

/**
 * test_refused_catalog:
 *
 * The catalog has to describe every image of the bundle, and may only
 * refer to icons and screenshots of the bundle.
 */
static void
test_refused_catalog(Fixture *f, gconstpointer user_data)
{
    /* an image without a component in the catalog, and one with the component of a different ID */
    mft_assert_refused(f,
                       &(BundleFlaws){.undescribed = &f->kit},
                       MF_ERROR_INVALID_BUNDLE,
                       "does not describe x86_64/" TEST_KIT_ID "/20260112T0238");
    mft_assert_refused(f,
                       &(BundleFlaws){.misdescribed = &f->kit},
                       MF_ERROR_INVALID_BUNDLE,
                       "describes org.example.Other, not " TEST_KIT_ID);

    /* an icon that is not a JPEG XL image */
    mft_assert_refused(f,
                       &(BundleFlaws){.bad_icon = TRUE},
                       MF_ERROR_INVALID_BUNDLE,
                       TEST_APP_ID ".jxl is not a JPEG XL image");

    /* screenshots that are not in the bundle */
    mft_assert_refused(f,
                       &(BundleFlaws){.screenshot_url = "https://example.org/screenshot.jxl"},
                       MF_ERROR_INVALID_BUNDLE,
                       "The screenshot https://example.org/screenshot.jxl of " TEST_APP_ID);
    mft_assert_refused(f,
                       &(BundleFlaws){.screenshot_url = TEST_APP_ID "/missing.jxl"},
                       MF_ERROR_INVALID_BUNDLE,
                       "The screenshot " TEST_APP_ID "/missing.jxl of " TEST_APP_ID);

    /* icons that are not in the bundle */
    mft_assert_refused(f,
                       &(BundleFlaws){.extra_icon = "<icon type=\"remote\" width=\"64\" height=\"64\">"
                                                    "https://example.org/icon.png</icon>"},
                       MF_ERROR_INVALID_BUNDLE,
                       "an icon of the type remote");
    mft_assert_refused(f,
                       &(BundleFlaws){.extra_icon = "<icon type=\"local\">/usr/share/pixmaps/app.png</icon>"},
                       MF_ERROR_INVALID_BUNDLE,
                       "an icon of the type local");
}

/**
 * test_not_a_bundle:
 *
 * A file that is not a ZIP archive with an index is not a bundle.
 */
static void
test_not_a_bundle(Fixture *f, gconstpointer user_data)
{
    g_autofree char *text_path = g_build_filename(f->dir, "notes.txt", NULL);
    g_autofree char *zip_path = g_build_filename(f->dir, "no-index.manifold", NULL);
    zip_t *zip = zip_open(zip_path, ZIP_CREATE | ZIP_TRUNCATE, NULL);

    mft_write_file(text_path, "hello\n");
    mft_assert_open_fails(text_path, MF_ERROR_INVALID_BUNDLE, "Not a bundle");

    mft_bundle_add(zip, "readme", "x", 1);
    g_assert_cmpint(zip_close(zip), ==, 0);
    mft_assert_open_fails(zip_path, MF_ERROR_INVALID_BUNDLE, "The bundle has no index.json");
}

/**
 * test_metadata_limit:
 *
 * A bundle can not make us read more than 64 MiB of metadata, even if each
 * file is small enough on its own.
 */
static void
test_metadata_limit(Fixture *f, gconstpointer user_data)
{
    const size_t filler_size = 16 * 1024 * 1024;
    g_autofree char *path = g_build_filename(f->dir, "large.manifold", NULL);
    g_autofree uint8_t *filler = g_malloc0(filler_size);
    g_autoptr(GString) index = mft_index_new();
    zip_t *zip = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, NULL);

    /* with the index, the fourth file is too much */
    for (guint i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("filler-%u", i);

        mft_add_file(zip, index, name, filler, filler_size);
    }
    mft_bundle_add_index(zip, index);
    g_assert_cmpint(zip_close(zip), ==, 0);

    mft_assert_open_fails(path, MF_ERROR_INVALID_BUNDLE, "larger than 64 MiB altogether");
}

/**
 * mft_read_zip_entry:
 *
 * Returns: (transfer full) (nullable): the content of a file in a bundle, or %NULL if the bundle
 *   has no file of the name
 */
static GBytes *
mft_read_zip_entry(const char *path, const char *name)
{
    zip_t *zip = zip_open(path, ZIP_RDONLY, NULL);
    zip_stat_t st;
    zip_file_t *file;
    char *data;

    g_assert_nonnull(zip);
    if (zip_stat(zip, name, 0, &st) != 0) {
        zip_discard(zip);
        return NULL;
    }
    file = zip_fopen(zip, name, 0);
    g_assert_nonnull(file);
    data = g_malloc(st.size);
    g_assert_cmpint(zip_fread(file, data, st.size), ==, st.size);
    zip_fclose(file);
    zip_discard(zip);

    return g_bytes_new_take(data, st.size);
}

/**
 * mft_check_bundle_signature:
 *
 * Check the signature of a bundle with ssh-keygen.
 *
 * Returns: (transfer full): the signature
 */
static GBytes *
mft_check_bundle_signature(Fixture *f, const char *path)
{
    g_autoptr(GBytes) index = mft_read_zip_entry(path, "index.json");
    GBytes *signature = mft_read_zip_entry(path, "index.json.sig");

    g_assert_nonnull(signature);
    mft_check_signature(f->dir, "manifold.bundle", index, signature);
    return signature;
}

/**
 * mft_sign_file:
 *
 * Sign a bundle in place, or remove its signature if there is no @signer.
 */
static gboolean
mft_sign_file(const char *path, MfSigner *signer, GError **error)
{
    g_autoptr(MfBundleWriter) writer = mf_bundle_writer_new_for_file(path, error);

    return writer != NULL && mf_bundle_writer_finish(writer, signer, error);
}

/**
 * test_sign_file:
 *
 * Sign a bundle in place, then replace and remove its signature.
 */
static void
test_sign_file(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_app_bundle(f, &(BundleFlaws){.unsigned_ = TRUE});
    g_autofree char *key = mft_make_key(f->dir, "key");
    g_autofree char *other_key = mft_make_key(f->dir, "other-key");
    g_autoptr(MfSigner) signer = mf_signer_new_for_key_file(key);
    g_autoptr(MfSigner) other_signer = mf_signer_new_for_key_file(other_key);
    g_autoptr(MfTrustedKeys) keys = mf_trusted_keys_new();
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(GBytes) signature = NULL;
    g_autoptr(GBytes) new_signature = NULL;
    g_autoptr(GBytes) no_signature = NULL;
    g_autofree char *fingerprint = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(mft_sign_file(path, signer, &error));
    g_assert_no_error(error);
    bundle = mft_open_bundle(path);
    g_assert_true(mf_bundle_is_signed(bundle));
    signature = mft_check_bundle_signature(f, path);

    /* the signature is valid for the key of the signer */
    g_assert_true(mf_trusted_keys_add_key(keys, mf_signer_get_public_key(signer), &error));
    g_assert_no_error(error);
    g_assert_true(mf_bundle_verify_signature(bundle, keys, &fingerprint, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(fingerprint, ==, mf_signer_get_fingerprint(signer));

    /* signing again replaces the signature, and the other key is not trusted */
    g_assert_true(mft_sign_file(path, other_signer, &error));
    g_assert_no_error(error);
    new_signature = mft_check_bundle_signature(f, path);
    g_assert_false(g_bytes_equal(signature, new_signature));
    g_clear_object(&bundle);
    bundle = mft_open_bundle(path);
    g_assert_false(mf_bundle_verify_signature(bundle, keys, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);

    /* signing without a signer removes the signature */
    g_assert_true(mft_sign_file(path, NULL, &error));
    g_assert_no_error(error);
    g_clear_object(&bundle);
    bundle = mft_open_bundle(path);
    g_assert_false(mf_bundle_is_signed(bundle));
    g_assert_false(mf_bundle_verify_signature(bundle, keys, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);

    /* a bundle that fails verification is not signed */
    g_clear_object(&bundle);
    g_free(path);
    path = mft_write_app_bundle(f, &(BundleFlaws){.unsigned_ = TRUE, .tampered = &f->app});
    g_assert_false(mft_sign_file(path, signer, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    no_signature = mft_read_zip_entry(path, "index.json.sig");
    g_assert_null(no_signature);
}

/**
 * mft_image_catalog:
 * @extra: (nullable): more XML for the component
 *
 * Returns: (transfer full): catalog XML for an image. The origin and the `bundle`
 *   element have wrong values on purpose, as the bundle writer has to replace them.
 */
static GBytes *
mft_image_catalog(const char *component_id, const char *extra)
{
    g_autofree char *component = mft_catalog_component_xml(component_id, "x86_64/wrong/1", extra);
    char *xml = g_strdup_printf("<components version=\"1.0\" origin=\"other\">%s</components>", component);

    return g_bytes_new_take(xml, strlen(xml));
}

/**
 * mft_write_image_file:
 *
 * Returns: (transfer full): the path of the file of an image
 */
static char *
mft_write_image_file(Fixture *f, const TestImage *image)
{
    g_autofree char *file_name = mft_image_file_name(image);
    char *path = g_build_filename(f->dir, file_name, NULL);

    mft_write_bytes(path, image->data);
    return path;
}

/**
 * mft_writer_add_image:
 * @extra: (nullable): more XML for the component
 *
 * Add an image to a bundle that is being written.
 */
static void
mft_writer_add_image(Fixture *f, MfBundleWriter *writer, const TestImage *image, const char *extra)
{
    g_autofree char *path = mft_write_image_file(f, image);
    char *json = mft_image_manifest_json(image, FALSE);
    g_autoptr(GBytes) manifest = g_bytes_new_take(json, strlen(json));
    g_autoptr(GBytes) catalog = mft_image_catalog(image->component_id, extra);
    g_autoptr(GError) error = NULL;

    g_assert_true(mf_bundle_writer_add_image(writer, path, manifest, catalog, &error));
    g_assert_no_error(error);
}

/**
 * mft_new_writer:
 *
 * Returns: (transfer full): a writer for a bundle in the test directory, which already contains the kit
 */
static MfBundleWriter *
mft_new_writer(Fixture *f, const char *path)
{
    g_autoptr(GError) error = NULL;
    MfBundleWriter *writer = mf_bundle_writer_new(path, &error);

    g_assert_no_error(error);
    mft_writer_add_image(f, writer, &f->kit, NULL);
    return writer;
}

/**
 * test_writer:
 *
 * Write a bundle with two images, an icon and a screenshot, and read it again.
 */
static void
test_writer(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = g_build_filename(f->dir, "written.manifold", NULL);
    g_autofree char *icons = g_build_filename(f->dir, "icons", NULL);
    g_autofree char *media = g_build_filename(f->dir, "media", NULL);
    g_autofree char *icon_dir = g_build_filename(icons, "64x64", NULL);
    g_autofree char *icon = g_build_filename(icon_dir, TEST_APP_ID ".jxl", NULL);
    g_autofree char *media_dir = g_build_filename(media, TEST_APP_ID, NULL);
    g_autofree char *screenshot = g_build_filename(media_dir, "screenshot.jxl", NULL);
    g_autoptr(MfBundleWriter) writer = mft_new_writer(f, path);
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(GBytes) catalog = NULL;
    g_autoptr(GBytes) index = NULL;
    g_autoptr(GError) error = NULL;
    MfBundleImage *app;
    gsize size;
    const char *text;

    mft_writer_add_image(f,
                         writer,
                         &f->app,
                         "<icon type=\"cached\" width=\"64\" height=\"64\">" TEST_APP_ID ".jxl</icon>");

    mft_make_dirs(icon_dir);
    mft_make_dirs(media_dir);
    g_assert_true(g_file_set_contents(icon, (const char *) JXL_DATA, sizeof(JXL_DATA), NULL));
    g_assert_true(g_file_set_contents(screenshot, (const char *) JXL_DATA, sizeof(JXL_DATA), NULL));
    g_assert_true(mf_bundle_writer_add_catalog_assets(writer, icons, media, &error));
    g_assert_no_error(error);

    /* the bundle file is not created before the writer finishes */
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    g_assert_true(mf_bundle_writer_finish(writer, NULL, &error));
    g_assert_no_error(error);

    /* reading the bundle returns the data we wrote */
    bundle = mft_open_bundle(path);
    g_assert_false(mf_bundle_is_signed(bundle));
    g_assert_cmpuint(mf_bundle_get_images(bundle)->len, ==, 2);
    app = mft_find_image(bundle, TEST_APP_ID);
    g_assert_cmpstr(mf_bundle_image_get_manifest_name(app), ==, TEST_APP_ID "_0.1.0.manifold.json");
    g_assert_cmpuint(g_hash_table_size(mf_bundle_image_get_icons(app)), ==, 1);
    g_assert_true(mf_bundle_verify_image(bundle, app, NULL, NULL, &error));
    g_assert_no_error(error);

    /* the writer set the origin of local bundles, and replaced the wrong `bundle` element */
    catalog = mft_read_zip_entry(path, "catalog/manifold-local.xml");
    text = g_bytes_get_data(catalog, &size);
    g_assert_nonnull(g_strstr_len(text, size, "origin=\"manifold-local\""));
    g_assert_null(g_strstr_len(text, size, "x86_64/wrong/1"));

    /* the index lists the manifests but not the images, as the manifests record the image hashes */
    index = mft_read_zip_entry(path, "index.json");
    text = g_bytes_get_data(index, &size);
    g_assert_nonnull(g_strstr_len(text, size, TEST_APP_ID "_0.1.0.manifold.json"));
    g_assert_null(g_strstr_len(text, size, TEST_APP_ID "_0.1.0.raw"));
}

/**
 * test_writer_sign:
 *
 * If signing fails, no bundle file is written, and the writer can be finished again.
 */
static void
test_writer_sign(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = g_build_filename(f->dir, "written.manifold", NULL);
    g_autofree char *bad_key = g_build_filename(f->dir, "bad-key", NULL);
    g_autofree char *key = mft_make_key(f->dir, "key");
    g_autoptr(MfBundleWriter) writer = mft_new_writer(f, path);
    g_autoptr(MfSigner) bad_signer = NULL;
    g_autoptr(MfSigner) signer = mf_signer_new_for_key_file(key);
    g_autoptr(GBytes) signature = NULL;
    g_autoptr(GError) error = NULL;

    mft_write_file(bad_key, "not a key\n");
    bad_signer = mf_signer_new_for_key_file(bad_key);
    g_assert_false(mf_bundle_writer_finish(writer, bad_signer, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

    g_assert_true(mf_bundle_writer_finish(writer, signer, &error));
    g_assert_no_error(error);
    signature = mft_check_bundle_signature(f, path);
}

/**
 * mft_assert_add_refused:
 * @message_part: a part of the expected error message
 *
 * Check that the writer refuses to add an image.
 */
static void
mft_assert_add_refused(MfBundleWriter *writer,
                       const char *image_file,
                       GBytes *manifest,
                       GBytes *catalog,
                       MfError code,
                       const char *message_part)
{
    g_autoptr(GError) error = NULL;

    g_assert_false(mf_bundle_writer_add_image(writer, image_file, manifest, catalog, &error));
    mft_assert_error(error, code, message_part);
}

/**
 * test_writer_refused:
 *
 * The writer refuses content that would make a bundle unreadable. A refused
 * call changes nothing, so the finished bundle only holds the accepted content.
 */
static void
test_writer_refused(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = g_build_filename(f->dir, "written.manifold", NULL);
    g_autofree char *app_file = mft_write_image_file(f, &f->app);
    g_autofree char *kit_file = mft_write_image_file(f, &f->kit);
    g_autofree char *app_xml = mft_catalog_component_xml(TEST_APP_ID, NULL, NULL);
    g_autofree char *kit_xml = mft_catalog_component_xml(TEST_KIT_ID, NULL, NULL);
    g_autofree char *both_xml = g_strconcat("<components>", app_xml, kit_xml, "</components>", NULL);
    g_autofree char *icons = g_build_filename(f->dir, "icons", NULL);
    g_autofree char *good_dir = g_build_filename(icons, "a", NULL);
    g_autofree char *bad_dir = g_build_filename(icons, "b", NULL);
    g_autofree char *good_file = g_build_filename(good_dir, "icon", NULL);
    g_autofree char *hidden_file = g_build_filename(bad_dir, ".hidden", NULL);
    char *json = mft_image_manifest_json(&f->app, FALSE);
    g_autoptr(GBytes) manifest = g_bytes_new_take(json, strlen(json));
    g_autoptr(GBytes) catalog = mft_image_catalog(TEST_APP_ID, NULL);
    g_autoptr(GBytes) kit_catalog = mft_image_catalog(TEST_KIT_ID, NULL);
    g_autoptr(GBytes) both_catalog = g_bytes_new_static(both_xml, strlen(both_xml));
    g_autoptr(MfBundleWriter) writer = mf_bundle_writer_new(path, NULL);
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(GError) error = NULL;

    /* a bundle without images */
    g_assert_false(mf_bundle_writer_finish(writer, NULL, &error));
    mft_assert_error(error, MF_ERROR_INVALID_BUNDLE, "The bundle contains no images");
    g_clear_error(&error);

    /* the manifest describes a different image */
    mft_assert_add_refused(writer, kit_file, manifest, catalog, MF_ERROR_VERIFICATION_FAILED, NULL);

    /* the catalog describes a different component, or two components */
    mft_assert_add_refused(writer,
                           app_file,
                           manifest,
                           kit_catalog,
                           MF_ERROR_INVALID_BUNDLE,
                           "The catalog data of " TEST_APP_ID " describes " TEST_KIT_ID);
    mft_assert_add_refused(writer, app_file, manifest, both_catalog, MF_ERROR_INVALID_BUNDLE, NULL);

    /* the same image twice */
    g_assert_true(mf_bundle_writer_add_image(writer, app_file, manifest, catalog, &error));
    g_assert_no_error(error);
    mft_assert_add_refused(writer, app_file, manifest, catalog, MF_ERROR_INVALID_BUNDLE, NULL);

    /* the first file of the icon tree is valid, but the writer must not keep it if a later file is refused */
    mft_make_dirs(good_dir);
    mft_make_dirs(bad_dir);
    mft_write_file(good_file, "icon");
    mft_write_file(hidden_file, "hidden");
    g_assert_false(mf_bundle_writer_add_catalog_assets(writer, icons, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_clear_error(&error);
    g_assert_cmpint(g_remove(hidden_file), ==, 0);
    g_assert_true(mf_bundle_writer_add_catalog_assets(writer, icons, NULL, &error));
    g_assert_no_error(error);

    g_assert_true(mf_bundle_writer_finish(writer, NULL, &error));
    g_assert_no_error(error);
    bundle = mft_open_bundle(path);
    g_assert_cmpuint(mf_bundle_get_images(bundle)->len, ==, 1);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

#define add_test(name, func) g_test_add("/bundle/" name, Fixture, NULL, mft_setup, func, mft_teardown)
    add_test("read", test_read);
    add_test("copy-image", test_copy_image);
    add_test("verify-image", test_verify_image);
    add_test("refused-files", test_refused_files);
    add_test("refused-catalog", test_refused_catalog);
    add_test("not-a-bundle", test_not_a_bundle);
    add_test("metadata-limit", test_metadata_limit);
    add_test("sign-file", test_sign_file);
    add_test("writer", test_writer);
    add_test("writer-sign", test_writer_sign);
    add_test("writer-refused", test_writer_refused);
#undef add_test

    return g_test_run();
}
