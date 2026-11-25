/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mft-utils.h"

#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <libglnx.h>
#include <zstd.h>

#include "mf-manifest-private.h"
#include "mf-store-private.h"
#include "mf-utils-private.h"

/**
 * mft_fake_hash:
 *
 * Create a fake hash from a seed.
 */
char *
mft_fake_hash(const char *seed)
{
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

void
mft_make_dirs(const char *path)
{
    g_assert_cmpint(g_mkdir_with_parents(path, 0755), ==, 0);
}

void
mft_make_link(const char *target, const char *path)
{
    g_assert_cmpint(symlink(target, path), ==, 0);
}

void
mft_replace_link(const char *target, const char *path)
{
    g_assert_cmpint(g_unlink(path), ==, 0);
    mft_make_link(target, path);
}

void
mft_write_file(const char *path, const char *content)
{
    g_assert_true(g_file_set_contents(path, content, -1, NULL));
}

char *
mft_read_file(const char *path)
{
    char *content = NULL;

    g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
    return content;
}

/**
 * mft_read_sample:
 *
 * Returns: (transfer full): the text of a file in the samples directory
 */
char *
mft_read_sample(const char *name)
{
    return mft_read_file(g_test_get_filename(G_TEST_DIST, "samples", name, NULL));
}

void
mft_write_bytes(const char *path, GBytes *bytes)
{
    g_assert_true(g_file_set_contents(path, g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), NULL));
}

/**
 * mft_str_replace:
 *
 * Returns: (transfer full): @text with every @old replaced by @new. The text has to contain @old.
 */
char *
mft_str_replace(const char *text, const char *old, const char *new)
{
    g_auto(GStrv) parts = g_strsplit(text, old, -1);

    g_assert_cmpuint(g_strv_length(parts), >, 1);
    return g_strjoinv(new, parts);
}

void
mft_replace_in_file(const char *path, const char *old, const char *new)
{
    g_autofree char *content = mft_read_file(path);
    g_autofree char *replaced = mft_str_replace(content, old, new);

    mft_write_file(path, replaced);
}

/**
 * mft_make_tmp_dir:
 *
 * Make a new temporary directory for the test of the given name.
 */
char *
mft_make_tmp_dir(const char *name)
{
    g_autofree char *template = g_strdup_printf("manifold-test-%s-XXXXXX", name);
    g_autoptr(GError) error = NULL;
    char *dir = g_dir_make_tmp(template, &error);

    g_assert_no_error(error);
    return dir;
}

void
mft_rm_rf(const char *path)
{
    g_autoptr(GError) error = NULL;

    g_assert_true(glnx_shutil_rm_rf_at(AT_FDCWD, path, NULL, &error));
    g_assert_no_error(error);
}

/**
 * mft_assert_error:
 * @code: the expected code of the error
 * @message_part: (nullable): a part of the expected error message
 *
 * Check that an operation failed with a certain #MfError.
 */
void
mft_assert_error(const GError *error, MfError code, const char *message_part)
{
    g_assert_error(error, MF_ERROR, (int) code);
    if (message_part != NULL && strstr(error->message, message_part) == NULL)
        g_error("The error message \"%s\" does not contain \"%s\"", error->message, message_part);
}

/**
 * mft_run:
 *
 * Run a command and fail the test if it fails.
 */
void
mft_run(const char *stdin_file, const char *const *argv)
{
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;

    if (stdin_file != NULL)
        g_subprocess_launcher_set_stdin_file_path(launcher, stdin_file);
    proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_assert_no_error(error);
    g_subprocess_wait_check(proc, NULL, &error);
    g_assert_no_error(error);
}

/**
 * mft_make_key:
 *
 * Make an unprotected ed25519 key pair in @dir.
 *
 * Returns: (transfer full): the path of the private key
 */
char *
mft_make_key(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    const char *argv[] = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", name, "-f", path, NULL};

    mft_make_dirs(dir);
    mft_run(NULL, argv);
    return path;
}

/**
 * mft_check_signature:
 *
 * Check that @signature is a valid signature over @data in the SSHSig namespace.
 * The data and the signature are written to @dir, as ssh-keygen only reads signatures from files.
 */
void
mft_check_signature(const char *dir, const char *sig_namespace, GBytes *data, GBytes *signature)
{
    g_autofree char *data_file = g_build_filename(dir, "data", NULL);
    g_autofree char *sig_file = g_build_filename(dir, "data.sig", NULL);
    const char *argv[] = {"ssh-keygen", "-Y", "check-novalidate", "-n", sig_namespace, "-s", sig_file, NULL};

    mft_write_bytes(data_file, data);
    mft_write_bytes(sig_file, signature);
    mft_run(data_file, argv);
}

/**
 * mft_sign:
 *
 * Sign @data with the private key at @key_path.
 *
 * Returns: (transfer full): the signature
 */
GBytes *
mft_sign(const char *key_path, MfSignatureScope scope, GBytes *data)
{
    g_autoptr(MfSigner) signer = mf_signer_new_for_key_file(key_path);
    g_autoptr(GError) error = NULL;
    GBytes *signature = mf_signer_sign(signer, scope, data, &error);

    g_assert_no_error(error);
    return signature;
}

/**
 * mft_blake3_hex:
 *
 * Returns: (transfer full): the BLAKE3 hash of @data, hex encoded
 */
char *
mft_blake3_hex(gconstpointer data, size_t size)
{
    g_autoptr(GBytes) bytes = g_bytes_new_static(data, size);
    MfFileEntry entry;

    mf_file_entry_set_for_bytes(&entry, bytes);
    return g_strdup(entry.blake3);
}

/**
 * mft_zstd:
 *
 * Returns: (transfer full): @text, compressed with zstd
 */
GBytes *
mft_zstd(const char *text)
{
    size_t bound = ZSTD_compressBound(strlen(text));
    g_autofree uint8_t *out = g_malloc(bound);
    size_t size = ZSTD_compress(out, bound, text, strlen(text), 3);

    g_assert_false(ZSTD_isError(size));
    return g_bytes_new(out, size);
}

/**
 * mft_clear_store:
 *
 * Remove everything from the store of the sandbox.
 */
void
mft_clear_store(void)
{
    g_autoptr(GDir) dir = g_dir_open(MF_STORAGE_DIR, 0, NULL);
    const char *name;

    g_assert_nonnull(dir);
    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree char *path = g_build_filename(MF_STORAGE_DIR, name, NULL);

        mft_rm_rf(path);
    }
}

/**
 * mft_catalog_component_xml:
 * @ref: (nullable): the value of the `bundle` element, "<architecture>/<component>/<version>",
 *   or %NULL to write no `bundle` element
 * @extra: (nullable): more XML for the component, e.g. its icons
 *
 * Write out a component of an AppStream catalog.
 */
char *
mft_catalog_component_xml(const char *component_id, const char *ref, const char *extra)
{
    g_autofree char *bundle = NULL;

    if (ref != NULL)
        bundle = g_strdup_printf("<bundle type=\"manifold\">%s</bundle>", ref);
    return g_strdup_printf("<component type=\"generic\"><id>%s</id><name>%s</name>%s%s</component>\n",
                           component_id,
                           component_id,
                           extra != NULL ? extra : "",
                           bundle != NULL ? bundle : "");
}

/**
 * mft_catalog_xml:
 * @components: the components, as written by mft_catalog_component_xml()
 *
 * Write out an AppStream catalog with the origin of local bundles.
 */
char *
mft_catalog_xml(const char *components)
{
    return g_strdup_printf(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<components version=\"1.0\" origin=\"manifold-local\">\n%s</components>\n",
        components);
}

/**
 * mft_manifest_base_json:
 * @section: the section to write, "base" or "base-sdk"
 *
 * Write out the reference to a base or a base SDK, as a section of a manifest.
 */
static char *
mft_manifest_base_json(const char *section,
                       const char *id,
                       const char *kind,
                       const char *version,
                       uint64_t revision)
{
    g_autofree char *identity = mft_fake_hash(id);

    g_assert_nonnull(kind);
    /* clang-format off */
    return g_strdup_printf(
        ",\n"
        "   \"%s\": {\n"
        "      \"component\": \"%s\",\n"
        "      \"kind\": \"%s\",\n"
        "      \"version\": \"%s\",\n"
        "      \"revision\": %" PRIu64 ",\n"
        "      \"identity\": \"%s\",\n"
        "      \"content-hash\": \"%s\"\n"
        "   }",
        section,
        id,
        kind,
        version != NULL ? version : "1",
        revision != 0 ? revision : 1,
        identity,
        identity);
    /* clang-format on */
}

/**
 * mft_manifest_json:
 *
 * Write out a manifest as JSON.
 */
char *
mft_manifest_json(const MftTestManifest *m)
{
    g_autofree char *identity = mft_fake_hash(m->component_id);
    g_autofree char *base = NULL;
    g_autofree char *base_sdk = NULL;
    g_autofree char *app = NULL;
    g_autofree char *integration = NULL;
    g_autofree char *permissions = NULL;

    if (m->base_id != NULL)
        base = mft_manifest_base_json("base", m->base_id, m->base_kind, m->base_version, m->base_revision);
    if (m->base_sdk_id != NULL)
        base_sdk = mft_manifest_base_json("base-sdk",
                                          m->base_sdk_id,
                                          m->base_sdk_kind,
                                          m->base_sdk_version,
                                          m->base_sdk_revision);
    if (g_strcmp0(m->kind, "app") == 0 && !m->sdk)
        app = g_strdup_printf(",\n   \"app\": {\n      \"command\": \"%s\"\n   }",
                              m->command != NULL ? m->command : "app");
    if (m->integration != NULL)
        integration = g_strdup_printf(",\n   \"integration\": %s", m->integration);
    if (m->permissions != NULL)
        permissions = g_strdup_printf(",\n   \"permissions\": %s", m->permissions);

    /* clang-format off */
    return g_strdup_printf(
        "{\n"
        "   \"format\": 1,\n"
        "   \"component\": \"%s\",\n"
        "   \"kind\": \"%s\",\n"
        "   \"version\": \"%s\",\n"
        "   \"revision\": %" PRIu64 ",\n"
        "   \"architecture\": \"%s\",\n"
        "   \"sdk\": %s,\n"
        "   \"identity\": \"%s\",\n"
        "   \"content-hash\": \"%s\"%s%s%s%s%s,\n"
        "   \"image\": {\n"
        "      \"file\": \"%s\",\n"
        "      \"size\": %zu,\n"
        "      \"sha256\": \"%s\",\n"
        "      \"blake3\": \"%s\",\n"
        "      \"verity-root-hash\": \"%s\",\n"
        "      \"data-offset\": %" PRIu64 ",\n"
        "      \"hash-offset\": %" PRIu64 "\n"
        "   }\n"
        "}",
        m->component_id,
        m->kind,
        m->version,
        m->revision != 0 ? m->revision : 1,
        m->architecture != NULL ? m->architecture : "x86_64",
        m->sdk ? "true" : "false",
        identity,
        m->content_hash != NULL ? m->content_hash : identity,
        base != NULL ? base : "",
        base_sdk != NULL ? base_sdk : "",
        app != NULL ? app : "",
        integration != NULL ? integration : "",
        permissions != NULL ? permissions : "",
        m->image_file != NULL ? m->image_file : "i.raw",
        m->image_size != 0 ? m->image_size : 1,
        m->sha256 != NULL ? m->sha256 : identity,
        m->blake3 != NULL ? m->blake3 : identity,
        m->verity_root_hash != NULL ? m->verity_root_hash : identity,
        m->data_offset != 0 ? m->data_offset : 20480,
        m->hash_offset != 0 ? m->hash_offset : 22528);
    /* clang-format on */
}

/**
 * mft_manifest_new:
 *
 * Returns: (transfer full): the parsed manifest
 */
MfManifest *
mft_manifest_new(const MftTestManifest *m)
{
    g_autofree char *json = mft_manifest_json(m);
    g_autoptr(GError) error = NULL;
    MfManifest *manifest = mf_manifest_new_from_data(json, -1, &error);

    g_assert_no_error(error);
    return manifest;
}

/**
 * mft_write_version:
 * @root: the store, e.g. %MF_STORAGE_DIR
 * @manifest: the manifest of the component
 * @base_version_dir: the version directory of the base that the `base` link points to,
 *   or %NULL for "current"
 * @mode: the install mode to record, or %NULL for none
 *
 * Write the version directory of a component the way the install daemon
 * creates it, with the image link, the `base` and `base-sdk` links and the
 * manifest. The version that is written last becomes the current version,
 * even if a newer version is installed.
 */
void
mft_write_version(const char *root,
                  const MftTestManifest *manifest,
                  const char *base_version_dir,
                  const char *mode)
{
    const char *subdir = mf_store_subdir_for_kind(mf_component_kind_from_string(manifest->kind),
                                                  manifest->sdk);
    g_autofree char *identity = mft_fake_hash(manifest->component_id);
    const char *hash = manifest->verity_root_hash != NULL ? manifest->verity_root_hash : identity;
    g_autofree char *component_dir = g_build_filename(root, subdir, manifest->component_id, NULL);
    g_autofree char *version_name = g_strdup_printf("%s-%" PRIu64,
                                                    manifest->version,
                                                    manifest->revision != 0 ? manifest->revision : 1);
    g_autofree char *version_dir = g_build_filename(component_dir, version_name, NULL);
    g_autofree char *image_link = g_build_filename(version_dir, mf_store_image_link_for_subdir(subdir), NULL);
    g_autofree char *image_target = g_strdup_printf("../../../" MF_STORE_IMAGES_SUBDIR "/%s.raw", hash);
    g_autofree char *image_name = g_strdup_printf("%s.raw", hash);
    g_autofree char *images_dir = g_build_filename(root, MF_STORE_IMAGES_SUBDIR, NULL);
    g_autofree char *image_path = g_build_filename(images_dir, image_name, NULL);
    g_autofree char *manifest_path = g_build_filename(version_dir, "manifest.json", NULL);
    g_autofree char *manifest_json = mft_manifest_json(manifest);
    g_autofree char *current_link = g_build_filename(component_dir, "current", NULL);

    mft_make_dirs(version_dir);
    mft_make_dirs(images_dir);
    mft_write_file(image_path, "not an image");
    mft_make_link(image_target, image_link);
    if (manifest->base_id != NULL) {
        g_autofree char *base_link = g_build_filename(version_dir, "base", NULL);
        g_autofree char *base_target = NULL;
        const char *base_subdir = MF_STORE_RUNTIMES_SUBDIR;

        /* the base of an SDK is the component of its own kind, any other component has a runtime as base */
        if (manifest->sdk)
            base_subdir = mf_store_subdir_for_kind(mf_component_kind_from_string(manifest->kind), FALSE);
        base_target = g_strdup_printf("../../../%s/%s/%s",
                                      base_subdir,
                                      manifest->base_id,
                                      base_version_dir != NULL ? base_version_dir : "current");
        mft_make_link(base_target, base_link);
    }
    if (manifest->base_sdk_id != NULL) {
        g_autofree char *link = g_build_filename(version_dir, "base-sdk", NULL);
        g_autofree char *target = g_strdup_printf(
            "../../../" MF_STORE_RUNTIMES_SUBDIR "/%s/%s-%" PRIu64,
            manifest->base_sdk_id,
            manifest->base_sdk_version != NULL ? manifest->base_sdk_version : "1",
            manifest->base_sdk_revision != 0 ? manifest->base_sdk_revision : 1);

        mft_make_link(target, link);
    }
    mft_write_file(manifest_path, manifest_json);
    if (mode != NULL) {
        g_autofree char *mode_path = g_build_filename(version_dir, "mode", NULL);

        mft_write_file(mode_path, mode);
    }
    g_unlink(current_link);
    mft_make_link(version_name, current_link);
}
