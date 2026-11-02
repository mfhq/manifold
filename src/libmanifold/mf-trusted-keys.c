/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-trusted-keys.h"

#include <string.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "mf-error.h"
#include "mf-signer-private.h"
#include "mf-utils-private.h"

/**
 * MfTrustedKeys:
 *
 * A set of SSH public keys whose signatures we trust.
 *
 * No key is trusted everywhere: whoever checks a signature decides which
 * keys to trust for it, for example the keys of one remote.
 */

/* a public key we trust */
typedef struct {
    char *public_key; /* "<type> <base64 key>", without the comment */
    char *fingerprint;
} MfTrustedKey;

struct _MfTrustedKeys {
    GObject parent_instance;

    GHashTable *keys; /* name -> MfTrustedKey */
};

G_DEFINE_FINAL_TYPE(MfTrustedKeys, mf_trusted_keys, G_TYPE_OBJECT)

static void
mf_trusted_key_free(MfTrustedKey *key)
{
    if (key == NULL)
        return;
    g_free(key->public_key);
    g_free(key->fingerprint);
    g_free(key);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfTrustedKey, mf_trusted_key_free)

static void
mf_trusted_keys_finalize(GObject *object)
{
    MfTrustedKeys *keys = MF_TRUSTED_KEYS(object);

    g_hash_table_unref(keys->keys);

    G_OBJECT_CLASS(mf_trusted_keys_parent_class)->finalize(object);
}

static void
mf_trusted_keys_class_init(MfTrustedKeysClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_trusted_keys_finalize;
}

static void
mf_trusted_keys_init(MfTrustedKeys *keys)
{
    keys->keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) mf_trusted_key_free);
}

/**
 * mf_trusted_keys_new:
 *
 * Create an empty set of trusted keys.
 *
 * Returns: (transfer full): a new #MfTrustedKeys
 */
MfTrustedKeys *
mf_trusted_keys_new(void)
{
    return g_object_new(MF_TYPE_TRUSTED_KEYS, NULL);
}

/**
 * mf_trusted_key_new:
 * @public_key: a public key in OpenSSH format, `<type> <base64 key> [comment]`
 *
 * Parse a public key. We only keep its type and the key itself, as we write
 * them into an allowed-signers file of ssh-keygen. There, any other text could
 * add options or keys of its own.
 *
 * Returns: (transfer full) (nullable): the key, or %NULL if @public_key is not a valid public key
 */
static MfTrustedKey *
mf_trusted_key_new(const char *public_key)
{
    g_autofree char *line = g_strstrip(g_strdup(public_key));
    g_autoptr(MfTrustedKey) key = NULL;
    g_auto(GStrv) parts = NULL;

    /* one key per file or string */
    if (strchr(line, '\n') != NULL)
        return NULL;
    parts = g_strsplit(line, " ", 3);
    if (g_strv_length(parts) < 2)
        return NULL;
    if (!mf_str_consists_of(parts[0], "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@.-") ||
        !mf_str_consists_of(parts[1], "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="))
        return NULL;

    key = g_new0(MfTrustedKey, 1);
    key->public_key = g_strdup_printf("%s %s", parts[0], parts[1]);
    key->fingerprint = mf_ssh_key_fingerprint(key->public_key);
    if (key->fingerprint == NULL)
        return NULL;

    return g_steal_pointer(&key);
}

/**
 * mf_trusted_keys_add_key:
 * @keys: an #MfTrustedKeys
 * @public_key: a public key in OpenSSH format, as it is stored in a `.pub` file
 * @error: a #GError
 *
 * Trust a public key.
 *
 * Returns: %TRUE if @public_key is a valid public key
 */
gboolean
mf_trusted_keys_add_key(MfTrustedKeys *keys, const char *public_key, GError **error)
{
    MfTrustedKey *key;

    g_return_val_if_fail(MF_IS_TRUSTED_KEYS(keys), FALSE);
    g_return_val_if_fail(public_key != NULL, FALSE);

    key = mf_trusted_key_new(public_key);
    if (key == NULL) {
        g_set_error_literal(error, MF_ERROR, MF_ERROR_FAILED, _("This is not a valid SSH public key"));
        return FALSE;
    }
    /* a key added this way is named by its fingerprint, which no key file is named like */
    g_hash_table_replace(keys->keys, g_strdup(key->fingerprint), key);

    return TRUE;
}

/**
 * mf_trusted_keys_add_dir:
 * @keys: an #MfTrustedKeys
 * @dir: a directory of public key files
 * @error: a #GError
 *
 * Trust the public keys in the `*.pub` files of @dir, which hold one key each.
 * A file replaces the key of the file with the same name in a directory that
 * was added earlier, and a symbolic link to `/dev/null` removes that key.
 * A directory that does not exist holds no keys.
 *
 * If a key file can not be read, nothing is added.
 *
 * Returns: %TRUE if every key file of @dir could be read
 */
gboolean
mf_trusted_keys_add_dir(MfTrustedKeys *keys, const char *dir, GError **error)
{
    g_autoptr(GHashTable) found = NULL;
    g_autoptr(GError) local_error = NULL;
    g_autoptr(GDir) gdir = NULL;
    GHashTableIter iter;
    gpointer name_ptr;
    gpointer key_ptr;
    const char *name;

    g_return_val_if_fail(MF_IS_TRUSTED_KEYS(keys), FALSE);
    g_return_val_if_fail(dir != NULL, FALSE);

    gdir = g_dir_open(dir, 0, &local_error);
    if (gdir == NULL) {
        if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            return TRUE;
        g_propagate_error(error, g_steal_pointer(&local_error));
        return FALSE;
    }

    /* read every key file first, so a broken one leaves @keys as it was */
    found = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) mf_trusted_key_free);
    while ((name = g_dir_read_name(gdir)) != NULL) {
        g_autofree char *path = NULL;
        g_autofree char *target = NULL;
        g_autofree char *contents = NULL;
        MfTrustedKey *key;

        if (name[0] == '.' || !g_str_has_suffix(name, MF_PUBLIC_KEY_SUFFIX))
            continue;
        path = g_build_filename(dir, name, NULL);

        /* a link to /dev/null masks the key of the same name, which we record as a key of NULL */
        target = g_file_read_link(path, NULL);
        if (g_strcmp0(target, "/dev/null") == 0) {
            g_hash_table_replace(found, g_strdup(name), NULL);
            continue;
        }

        if (!g_file_get_contents(path, &contents, NULL, error))
            return FALSE;
        key = mf_trusted_key_new(contents);
        if (key == NULL) {
            g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("%s does not hold a valid SSH public key"), path);
            return FALSE;
        }
        g_hash_table_replace(found, g_strdup(name), key);
    }

    /* the keys we found move over to @keys */
    g_hash_table_iter_init(&iter, found);
    while (g_hash_table_iter_next(&iter, &name_ptr, &key_ptr)) {
        g_hash_table_iter_steal(&iter);
        if (key_ptr != NULL) {
            g_hash_table_replace(keys->keys, name_ptr, key_ptr);
        } else {
            g_hash_table_remove(keys->keys, name_ptr);
            g_free(name_ptr);
        }
    }

    return TRUE;
}

/**
 * mf_trusted_keys_is_empty:
 * @keys: an #MfTrustedKeys
 *
 * Returns: %TRUE if no key is trusted
 */
gboolean
mf_trusted_keys_is_empty(MfTrustedKeys *keys)
{
    g_return_val_if_fail(MF_IS_TRUSTED_KEYS(keys), TRUE);
    return g_hash_table_size(keys->keys) == 0;
}

/**
 * mf_bytes_to_stripped_string:
 *
 * Returns: (transfer full): the text of @bytes, without leading and trailing whitespace
 */
static char *
mf_bytes_to_stripped_string(GBytes *bytes)
{
    gsize size = 0;
    const char *data = g_bytes_get_data(bytes, &size);

    /* empty bytes may have no data at all */
    return g_strstrip(size > 0 ? g_strndup(data, size) : g_strdup(""));
}

/**
 * mf_run_ssh_keygen:
 * @argv: the arguments of ssh-keygen, starting with its path
 * @input: (nullable): what ssh-keygen reads on stdin
 * @success: (out): whether ssh-keygen succeeded
 * @output: (out): what ssh-keygen wrote to stdout
 * @message: (out): what ssh-keygen wrote to stderr
 *
 * Returns: %FALSE if ssh-keygen could not be run at all
 */
static gboolean
mf_run_ssh_keygen(const char *const *argv,
                  GBytes *input,
                  gboolean *success,
                  char **output,
                  char **message,
                  GError **error)
{
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GBytes) no_input = NULL;
    g_autoptr(GBytes) stdout_data = NULL;
    g_autoptr(GBytes) stderr_data = NULL;

    /* ssh-keygen must never wait for input from a terminal */
    if (input == NULL)
        input = no_input = g_bytes_new_static("", 0);

    proc = g_subprocess_newv(argv,
                             G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_PIPE,
                             error);
    if (proc == NULL)
        return FALSE;
    if (!g_subprocess_communicate(proc, input, NULL, &stdout_data, &stderr_data, error))
        return FALSE;

    *success = g_subprocess_get_successful(proc);
    *output = mf_bytes_to_stripped_string(stdout_data);
    *message = mf_bytes_to_stripped_string(stderr_data);
    return TRUE;
}

/**
 * mf_trusted_keys_verify_in_dir:
 * @tmp_dir: a private directory for the files ssh-keygen reads
 *
 * Check a signature with ssh-keygen. ssh-keygen first finds the trusted key
 * that made the signature, and then checks the signature with that key.
 */
static gboolean
mf_trusted_keys_verify_in_dir(MfTrustedKeys *keys,
                              const char *tmp_dir,
                              const char *sig_namespace,
                              GBytes *data,
                              GBytes *signature,
                              char **fingerprint,
                              GError **error)
{
    g_autofree char *ssh_keygen = NULL;
    g_autofree char *allowed_file = g_build_filename(tmp_dir, "allowed_signers", NULL);
    g_autofree char *sig_file = g_build_filename(tmp_dir, "signature", NULL);
    g_autoptr(GString) allowed = g_string_new(NULL);
    g_autofree char *principal = NULL;
    g_autofree char *message = NULL;
    g_autofree char *output = NULL;
    GHashTableIter iter;
    gpointer key_ptr;
    gboolean success = FALSE;
    gboolean ret = FALSE;

    ssh_keygen = g_find_program_in_path("ssh-keygen");
    if (ssh_keygen == NULL) {
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_FAILED,
                            _("Checking signatures needs ssh-keygen, which is not installed"));
        return FALSE;
    }

    /* every key is its own principal, named by its fingerprint, and only valid for the namespace */
    g_hash_table_iter_init(&iter, keys->keys);
    while (g_hash_table_iter_next(&iter, NULL, &key_ptr)) {
        const MfTrustedKey *key = key_ptr;

        g_string_append_printf(allowed,
                               "%s namespaces=\"%s\" %s\n",
                               key->fingerprint,
                               sig_namespace,
                               key->public_key);
    }
    if (!g_file_set_contents_full(allowed_file,
                                  allowed->str,
                                  allowed->len,
                                  G_FILE_SET_CONTENTS_NONE,
                                  0600,
                                  error))
        goto out;
    if (!g_file_set_contents_full(sig_file,
                                  g_bytes_get_data(signature, NULL),
                                  g_bytes_get_size(signature),
                                  G_FILE_SET_CONTENTS_NONE,
                                  0600,
                                  error))
        goto out;

    /* find the trusted key that made the signature */
    if (!mf_run_ssh_keygen(
            (const char *[]){ssh_keygen, "-Y", "find-principals", "-s", sig_file, "-f", allowed_file, NULL},
            NULL,
            &success,
            &output,
            &message,
            error))
        goto out;
    if (!success || output[0] == '\0') {
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_VERIFICATION_FAILED,
                            _("The signature was not made by a trusted key"));
        goto out;
    }
    /* a key that is trusted twice is listed twice */
    principal = g_strndup(output, strcspn(output, "\n"));
    g_clear_pointer(&output, g_free);
    g_clear_pointer(&message, g_free);

    /* check the signature itself, which also checks its namespace */
    if (!mf_run_ssh_keygen((const char *[]){ssh_keygen,
                                            "-Y",
                                            "verify",
                                            "-f",
                                            allowed_file,
                                            "-I",
                                            principal,
                                            "-n",
                                            sig_namespace,
                                            "-s",
                                            sig_file,
                                            NULL},
                           data,
                           &success,
                           &output,
                           &message,
                           error))
        goto out;
    if (!success) {
        /* ssh-keygen ends its message with a line that only says that it failed */
        message[strcspn(message, "\n")] = '\0';
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("The signature is not valid: %s"),
                    message);
        goto out;
    }

    if (fingerprint != NULL)
        *fingerprint = g_steal_pointer(&principal);
    ret = TRUE;

out:
    g_unlink(allowed_file);
    g_unlink(sig_file);
    return ret;
}

/**
 * mf_trusted_keys_verify:
 * @keys: an #MfTrustedKeys
 * @scope: what the signature has to be for
 * @data: the data that was signed
 * @signature: the armored SSHSig signature
 * @fingerprint: (out) (optional) (transfer full): the fingerprint of the key that made the signature
 * @error: a #GError
 *
 * Check that @signature is a signature over @data for @scope, made by one of @keys.
 *
 * Returns: %TRUE if the signature is valid, and a trusted key made it
 */
gboolean
mf_trusted_keys_verify(MfTrustedKeys *keys,
                       MfSignatureScope scope,
                       GBytes *data,
                       GBytes *signature,
                       char **fingerprint,
                       GError **error)
{
    const char *sig_namespace = mf_signature_scope_to_namespace(scope);
    g_autofree char *tmp_dir = NULL;
    gboolean ret;

    g_return_val_if_fail(MF_IS_TRUSTED_KEYS(keys), FALSE);
    g_return_val_if_fail(sig_namespace != NULL, FALSE);
    g_return_val_if_fail(data != NULL, FALSE);
    g_return_val_if_fail(signature != NULL, FALSE);

    if (g_hash_table_size(keys->keys) == 0) {
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_VERIFICATION_FAILED,
                            _("No key is trusted, so the signature can not be checked"));
        return FALSE;
    }

    /* ssh-keygen reads the trusted keys and the signature from files */
    tmp_dir = g_dir_make_tmp("manifold-verify-XXXXXX", error);
    if (tmp_dir == NULL)
        return FALSE;
    ret = mf_trusted_keys_verify_in_dir(keys, tmp_dir, sig_namespace, data, signature, fingerprint, error);
    g_rmdir(tmp_dir);

    return ret;
}
