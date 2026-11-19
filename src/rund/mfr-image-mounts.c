/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfr-image-mounts.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/loop.h>
#include <libcryptsetup.h>
#include <libglnx.h>


constexpr char DEVICE_PREFIX[] = "manifold-";
constexpr char MAPPER_DIR[] = "/dev/mapper";
constexpr char LOOP_CONTROL[] = "/dev/loop-control";
constexpr size_t ROOT_HASH_SIZE = 32;

/**
 * MfrImageMount:
 *
 * The filesystem of an image, mounted with dm-verity at
 * MFR_IMAGES_MOUNT_DIR/<hash>/. A mount is shared between the trees that
 * use the image and has a user count. Every user that acquired the mount
 * has to release it with mfr_image_mounts_release().
 *
 * An image is a disk image with a filesystem partition, a dm-verity hash
 * partition and a signature partition. We do not let the kernel scan the
 * image for partitions. Instead, we attach the image to a loop device that
 * starts at the filesystem partition, and pass the offset of the hash
 * partition within the loop device to dm-verity. Then we mount the verity device.
 */
struct MfrImageMount {
    char *verity_hash;
    char *mountpoint;
    unsigned int users;
};

/**
 * MfrImageMounts:
 *
 * The table of mounted images, indexed by their verity hash. All images are
 * mounted via this table, so an image that several trees use is only
 * mounted once.
 */
struct _MfrImageMounts {
    GObject parent_instance;

    GHashTable *table; /* verity hash -> MfrImageMount */
};

G_DEFINE_FINAL_TYPE(MfrImageMounts, mfr_image_mounts, G_TYPE_OBJECT)

/**
 * mfr_image_mount_free:
 */
static void
mfr_image_mount_free(MfrImageMount *img_mount)
{
    g_free(img_mount->verity_hash);
    g_free(img_mount->mountpoint);
    g_free(img_mount);
}

/**
 * mfr_cryptsetup_log:
 */
static void
mfr_cryptsetup_log(int level, const char *message, void *user_data)
{
    g_autofree char *trimmed = g_strchomp(g_strdup(message));

    if (level == CRYPT_LOG_ERROR)
        g_warning("cryptsetup: %s", trimmed);
    else
        g_debug("cryptsetup: %s", trimmed);
}

/**
 * mfr_loop_attach:
 *
 * Attach an image to a loop device, read-only, starting at the given offset.
 * The kernel detaches the device automatically once it is no longer in use.
 *
 * Returns: a file descriptor of the loop device, or -1 on error. The path of
 *   the device is stored in @ret_path.
 */
static int
mfr_loop_attach(int image_fd, guint64 offset, char **ret_path, GError **error)
{
    g_autofd int control_fd = -1;

    control_fd = open(LOOP_CONTROL, O_RDWR | O_CLOEXEC);
    if (control_fd < 0)
        return glnx_throw_errno_prefix(error, "Unable to open %s", LOOP_CONTROL), -1;

    /* someone else may take the free device before we configure it */
    for (int attempt = 0; attempt < 16; attempt++) {
        g_autofree char *path = NULL;
        g_autofd int loop_fd = -1;
        struct loop_config config = {
            .fd = (__u32) image_fd,
            .info = {.lo_offset = offset, .lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR},
        };

        int number = ioctl(control_fd, LOOP_CTL_GET_FREE);
        if (number < 0)
            return glnx_throw_errno_prefix(error, "Unable to find a free loop device"), -1;

        path = g_strdup_printf("/dev/loop%d", number);
        loop_fd = open(path, O_RDONLY | O_CLOEXEC);
        if (loop_fd < 0)
            return glnx_throw_errno_prefix(error, "Unable to open %s", path), -1;

        if (ioctl(loop_fd, LOOP_CONFIGURE, &config) == 0) {
            *ret_path = g_steal_pointer(&path);
            return g_steal_fd(&loop_fd);
        }
        if (errno != EBUSY)
            return glnx_throw_errno_prefix(error, "Unable to attach image to %s", path), -1;
    }

    return glnx_throw(error, "Unable to attach image: every free loop device was taken by another process"),
           -1;
}

/**
 * mfr_decode_root_hash:
 *
 * Decode a verity root hash from a string.
 */
static gboolean
mfr_decode_root_hash(const char *verity_hash, guint8 *ret_hash)
{
    for (size_t i = 0; i < ROOT_HASH_SIZE; i++) {
        int high = g_ascii_xdigit_value(verity_hash[2 * i]);
        int low = g_ascii_xdigit_value(verity_hash[2 * i + 1]);

        if (high < 0 || low < 0)
            return FALSE;
        ret_hash[i] = (guint8) ((high << 4) | low);
    }

    return TRUE;
}

/**
 * mfr_verity_activate:
 *
 * Set up the verity device for an image that is attached to a loop device.
 */
static gboolean
mfr_verity_activate(const char *loop_path, const MfrImage *image, const char *device_name, GError **error)
{
    struct crypt_device *cd = NULL;
    struct crypt_params_verity params = {
        /* the hash partition follows the filesystem partition in the same device */
        .hash_area_offset = image->hash_offset - image->data_offset,
    };
    guint8 root_hash[ROOT_HASH_SIZE];
    gboolean ret = FALSE;
    int r;

    if (!mfr_decode_root_hash(image->verity_hash, root_hash))
        return glnx_throw(error, "Root hash of %s is not valid: %s", image->image_path, image->verity_hash);

    r = crypt_init(&cd, loop_path);
    if (r < 0)
        return glnx_throw(error, "Unable to use %s with cryptsetup: %s", loop_path, g_strerror(-r));
    crypt_set_log_callback(cd, mfr_cryptsetup_log, NULL);

    r = crypt_load(cd, CRYPT_VERITY, &params);
    if (r < 0) {
        glnx_throw(error,
                   "Unable to read the verity superblock of %s: %s",
                   image->image_path,
                   g_strerror(-r));
        goto out;
    }

    /* TODO: Pass the signature partition to the kernel once repositories sign
     * images. So far, we only enforce the root hash. */
    r = crypt_activate_by_volume_key(cd,
                                     device_name,
                                     (const char *) root_hash,
                                     sizeof(root_hash),
                                     CRYPT_ACTIVATE_READONLY | CRYPT_ACTIVATE_PRIVATE);
    if (r < 0) {
        glnx_throw(error, "Unable to set up dm-verity for %s: %s", image->image_path, g_strerror(-r));
        goto out;
    }

    ret = TRUE;
out:
    crypt_free(cd);
    return ret;
}

/**
 * mfr_verity_deactivate:
 *
 * Remove a verity device. The removal is deferred until the device is no
 * longer in use, because an application that was launched earlier may still
 * have it mounted.
 */
static gboolean
mfr_verity_deactivate(const char *device_name, GError **error)
{
    int r = crypt_deactivate_by_name(NULL, device_name, CRYPT_DEACTIVATE_DEFERRED);
    if (r < 0 && r != -ENODEV)
        return glnx_throw(error, "Unable to remove %s: %s", device_name, g_strerror(-r));
    return TRUE;
}

/**
 * mfr_image_open:
 *
 * Open an image by its path in the store, without following a symbolic link.
 *
 * Returns: a file descriptor, or -1 on error
 */
static int
mfr_image_open(const char *path, GError **error)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return glnx_throw_errno_prefix(error, "Unable to open image %s", path), -1;

    return fd;
}

/**
 * mfr_image_setup:
 *
 * Set up the verity device for an image. An existing device for the image
 * is reused. The name of a device contains the root hash that it enforces,
 * and only root can create devices.
 */
static gboolean
mfr_image_setup(const MfrImage *image, const char *device_name, const char *device_path, GError **error)
{
    g_autofd int image_fd = -1;
    g_autofd int loop_fd = -1;
    g_autofree char *loop_path = NULL;

    if (g_file_test(device_path, G_FILE_TEST_EXISTS)) {
        g_debug("Reusing %s for %s", device_path, image->image_path);
        /* cancel the deferred removal, which is pending if the last tree using the device was dropped */
        crypt_deactivate_by_name(NULL, device_name, CRYPT_DEACTIVATE_DEFERRED_CANCEL);
        return TRUE;
    }

    image_fd = mfr_image_open(image->image_path, error);
    if (image_fd < 0)
        return FALSE;
    loop_fd = mfr_loop_attach(image_fd, image->data_offset, &loop_path, error);
    if (loop_fd < 0)
        return FALSE;

    /* if the activation fails, the loop device is detached when we close its file descriptor */
    return mfr_verity_activate(loop_path, image, device_name, error);
}

/**
 * mfr_image_mounts_acquire:
 * @image: the image to mount
 *
 * Mount an image, or reuse its mount if it is mounted already. Every acquired
 * mount has to be released with mfr_image_mounts_release().
 *
 * Returns: (transfer none): the mount, owned by @mounts, or %NULL on error
 */
MfrImageMount *
mfr_image_mounts_acquire(MfrImageMounts *mounts, const MfrImage *image, GError **error)
{
    g_autofree char *device_name = NULL;
    g_autofree char *device_path = NULL;
    g_autofree char *path = NULL;

    MfrImageMount *img_mount = g_hash_table_lookup(mounts->table, image->verity_hash);
    if (img_mount != NULL) {
        img_mount->users++;
        return img_mount;
    }

    device_name = g_strconcat(DEVICE_PREFIX, image->verity_hash, NULL);
    device_path = g_build_filename(MAPPER_DIR, device_name, NULL);
    path = g_build_filename(MFR_IMAGES_MOUNT_DIR, image->verity_hash, NULL);

    if (!mfr_image_setup(image, device_name, device_path, error))
        return NULL;
    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, path, 0755, NULL, error))
        goto fail;
    if (mount(device_path, path, "erofs", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) != 0) {
        glnx_throw_errno_prefix(error, "Unable to mount %s at %s", device_path, path);
        goto fail;
    }
    g_debug("Mounted image %s at %s", image->image_path, path);

    img_mount = g_new0(MfrImageMount, 1);
    img_mount->verity_hash = g_strdup(image->verity_hash);
    img_mount->mountpoint = g_steal_pointer(&path);
    img_mount->users = 1;
    g_hash_table_insert(mounts->table, img_mount->verity_hash, img_mount);

    return img_mount;

fail:
    (void) rmdir(path);
    mfr_verity_deactivate(device_name, NULL);
    return NULL;
}

/**
 * mfr_image_mounts_release:
 * @img_mount: (nullable): a mount that was returned by mfr_image_mounts_acquire()
 *
 * Release a mount. The caller must not use the mount afterwards. When the
 * last user has released the mount, the image is unmounted. Its verity device
 * is removed as soon as no running application uses it anymore.
 */
void
mfr_image_mounts_release(MfrImageMounts *mounts, MfrImageMount *img_mount)
{
    g_autofree char *device_name = NULL;
    g_autoptr(GError) error = NULL;

    if (img_mount == NULL)
        return;
    g_return_if_fail(img_mount->users > 0);
    if (--img_mount->users > 0)
        return;

    device_name = g_strconcat(DEVICE_PREFIX, img_mount->verity_hash, NULL);
    if (umount2(img_mount->mountpoint, MNT_DETACH) != 0)
        g_warning("Unable to unmount %s: %s", img_mount->mountpoint, g_strerror(errno));
    if (rmdir(img_mount->mountpoint) != 0)
        g_warning("Unable to remove %s: %s", img_mount->mountpoint, g_strerror(errno));
    if (!mfr_verity_deactivate(device_name, &error))
        g_warning("%s", error->message);
    g_debug("Unmounted image %s", img_mount->verity_hash);

    /* removing the entry frees the mount, including the verity hash that is the key */
    g_hash_table_remove(mounts->table, img_mount->verity_hash);
}

/**
 * mfr_image_mount_get_path:
 *
 * Returns: the path at which the filesystem of the image is mounted
 */
const char *
mfr_image_mount_get_path(const MfrImageMount *img_mount)
{
    return img_mount->mountpoint;
}

/**
 * mfr_image_mounts_remove_stale_devices:
 *
 * Remove the verity devices that a previous run of the daemon left behind.
 * This function has to be called when the daemon starts, before it sets up
 * any device of its own. If a running application still uses a device, the
 * device is removed once the application exits, unless we reuse it first.
 */
void
mfr_image_mounts_remove_stale_devices(void)
{
    g_autoptr(GDir) entries = g_dir_open(MAPPER_DIR, 0, NULL);
    const char *name;

    while (entries != NULL && (name = g_dir_read_name(entries)) != NULL) {
        g_autoptr(GError) error = NULL;

        if (!g_str_has_prefix(name, DEVICE_PREFIX))
            continue;
        g_info("Removing device %s once nothing uses it", name);
        if (!mfr_verity_deactivate(name, &error))
            g_warning("%s", error->message);
    }
}

/**
 * mfr_image_mounts_init:
 */
static void
mfr_image_mounts_init(MfrImageMounts *mounts)
{
    /* NOTE: The key is the verity hash that the mount owns, so we have no key destroy function. */
    mounts->table = g_hash_table_new_full(g_str_hash,
                                          g_str_equal,
                                          NULL,
                                          (GDestroyNotify) mfr_image_mount_free);
}

/**
 * mfr_image_mounts_finalize:
 */
static void
mfr_image_mounts_finalize(GObject *object)
{
    MfrImageMounts *mounts = MFR_IMAGE_MOUNTS(object);

    g_hash_table_unref(mounts->table);

    G_OBJECT_CLASS(mfr_image_mounts_parent_class)->finalize(object);
}

/**
 * mfr_image_mounts_class_init:
 */
static void
mfr_image_mounts_class_init(MfrImageMountsClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mfr_image_mounts_finalize;
}

/**
 * mfr_image_mounts_new:
 *
 * Create a new, empty table of image mounts.
 *
 * Returns: (transfer full): a new table of image mounts
 */
MfrImageMounts *
mfr_image_mounts_new(void)
{
    return g_object_new(MFR_TYPE_IMAGE_MOUNTS, NULL);
}
