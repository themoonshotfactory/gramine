/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2021 Intel Corporation
 *                    Paweł Marczewski <pawel@invisiblethingslab.com>
 */

/*
 * This file contains code for implementation of 'chroot' filesystem.
 *
 * TODO: reintroduce the file position sync (using shim_fs_sync.h) after the migration to inodes is
 * finished.
 */

#define _POSIX_C_SOURCE 200809L /* for SSIZE_MAX */
#include <asm/mman.h>
#include <errno.h>
#include <limits.h>
#include <linux/fcntl.h>

#include "pal.h"
#include "perm.h"
#include "shim_flags_conv.h"
#include "shim_fs.h"
#include "shim_handle.h"
#include "shim_internal.h"
#include "shim_lock.h"
#include "shim_utils.h"
#include "stat.h"

#define KEEP_URI_PREFIX 0

/*
 * Always add a read permission to files created on host, because PAL requires opening the file even
 * for operations such as `unlink` or `chmod`.
 *
 * The updated file permissions will not be visible to the process creating the file or updating its
 * permissions, e.g. if a process creates a write-only file, Gramine's `stat` will still report it
 * as write-only. However, other Gramine processes accessing that file afterwards will see the
 * updated permissions.
 */
#define HOST_PERM(perm) ((perm) | PERM_r________)

static int chroot_mount(const char* uri, void** mount_data) {
    __UNUSED(mount_data);
    if (!(strstartswith(uri, URI_PREFIX_FILE) || strstartswith(uri, URI_PREFIX_DEV)))
        return -EINVAL;
    return 0;
}

static const char* strip_prefix(const char* uri) {
    const char* s = strchr(uri, ':');
    assert(s);
    return s + 1;
}

/*
 * Calculate the URI for a dentry. The URI scheme is determined by file type (`type` field). It
 * needs to be passed separately (instead of using `dent->inode->type`) because the dentry might not
 * have inode associated yet: we might be creating a new file, or looking up a file we don't know
 * yet.
 *
 * If `type` is KEEP_URI_PREFIX, we keep the URI prefix from mount URI.
 */
static int chroot_dentry_uri(struct shim_dentry* dent, mode_t type, char** out_uri) {
    assert(dent->mount);
    assert(dent->mount->uri);

    int ret;

    const char* root = strip_prefix(dent->mount->uri);

    const char* prefix;
    size_t prefix_len;
    switch (type) {
        case S_IFREG:
            prefix = URI_PREFIX_FILE;
            prefix_len = static_strlen(URI_PREFIX_FILE);
            break;
        case S_IFDIR:
            prefix = URI_PREFIX_DIR;
            prefix_len = static_strlen(URI_PREFIX_DIR);
            break;
        case S_IFCHR:
            prefix = URI_PREFIX_DEV;
            prefix_len = static_strlen(URI_PREFIX_DEV);
            break;
        case KEEP_URI_PREFIX:
            prefix = dent->mount->uri;
            prefix_len = root - prefix;
            break;
        default:
            BUG();
    }

    char* rel_path;
    size_t rel_path_size;
    ret = dentry_rel_path(dent, &rel_path, &rel_path_size);
    if (ret < 0)
        return ret;

    /* Treat empty path as "." */
    if (*root == '\0')
        root = ".";

    size_t root_len = strlen(root);

    /* Allocate buffer for "<prefix:><root>/<rel_path>" (if `rel_path` is empty, we don't need the
     * space for `/`, but overallocating 1 byte doesn't hurt us, and keeps the code simple) */
    char* uri = malloc(prefix_len + root_len + 1 + rel_path_size);
    if (!uri) {
        ret = -ENOMEM;
        goto out;
    }
    memcpy(uri, prefix, prefix_len);
    memcpy(uri + prefix_len, root, root_len);
    if (rel_path_size == 1) {
        /* this is the mount root, the URI is "<prefix:><root>"*/
        uri[prefix_len + root_len] = '\0';
    } else {
        /* this is not the mount root, the URI is "<prefix:><root>/<rel_path>" */
        uri[prefix_len + root_len] = '/';
        memcpy(uri + prefix_len + root_len + 1, rel_path, rel_path_size);
    }
    *out_uri = uri;
    ret = 0;

out:
    free(rel_path);
    return ret;
}

static int chroot_setup_dentry(struct shim_dentry* dent, mode_t type, mode_t perm,
                               file_off_t size) {
    assert(locked(&g_dcache_lock));
    assert(!dent->inode);

    struct shim_inode* inode = get_new_inode(dent->mount, type, perm);
    if (!inode)
        return -ENOMEM;
    inode->size = size;
    dent->inode = inode;
    return 0;
}

static int chroot_lookup(struct shim_dentry* dent) {
    assert(locked(&g_dcache_lock));

    int ret;

    /*
     * We don't know the file type yet, so we can't construct a PAL URI with the right prefix. Use
     * the file type from mount URI.
     *
     * Explanation: In almost all cases, a "file:" URI would be good enough. If the underlying file
     * is a directory or a device, `DkStreamAttributesQuery` will still recognize it. However, PAL
     * also recognizes a special "dev:tty" device, which doesn't work that way (i.e. "file:tty" will
     * not open it).
     */
    char* uri = NULL;
    ret = chroot_dentry_uri(dent, KEEP_URI_PREFIX, &uri);
    if (ret < 0)
        goto out;

    PAL_STREAM_ATTR pal_attr;
    ret = DkStreamAttributesQuery(uri, &pal_attr);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }

    mode_t type;
    switch (pal_attr.handle_type) {
        case PAL_TYPE_FILE:
            type = S_IFREG;
            break;
        case PAL_TYPE_DIR:
            type = S_IFDIR;
            break;
        case PAL_TYPE_DEV:
            type = S_IFCHR;
            break;
        case PAL_TYPE_PIPE:
            log_warning("trying to access '%s' which is a host-level FIFO (named pipe); "
                        "Gramine supports only named pipes created by Gramine processes",
                        uri);
            ret = -EACCES;
            goto out;
        default:
            log_error("unexpected handle type returned by PAL: %d", pal_attr.handle_type);
            BUG();
    }

    mode_t perm = pal_attr.share_flags;

    file_off_t size = (type == S_IFREG ? pal_attr.pending_size : 0);

    ret = chroot_setup_dentry(dent, type, perm, size);
out:
    free(uri);
    return ret;
}

/* Open a temporary read-only PAL handle for a file (used by `unlink` etc.) */
static int chroot_temp_open(struct shim_dentry* dent, mode_t type, PAL_HANDLE* out_palhdl) {
    char* uri;
    int ret = chroot_dentry_uri(dent, type, &uri);
    if (ret < 0)
        return ret;

    ret = DkStreamOpen(uri, PAL_ACCESS_RDONLY, /*share_flags=*/0, PAL_CREATE_NEVER,
                       /*options=*/0, out_palhdl);
    free(uri);
    return pal_to_unix_errno(ret);
}

/* Open a PAL handle, and associate it with a LibOS handle (if provided). */
static int chroot_do_open(struct shim_handle* hdl, struct shim_dentry* dent, mode_t type,
                          int flags, mode_t perm) {
    assert(locked(&g_dcache_lock));

    int ret;

    char* uri;
    ret = chroot_dentry_uri(dent, type, &uri);
    if (ret < 0)
        return ret;

    PAL_HANDLE palhdl;
    enum pal_access access = LINUX_OPEN_FLAGS_TO_PAL_ACCESS(flags);
    enum pal_create_mode create = LINUX_OPEN_FLAGS_TO_PAL_CREATE(flags);
    pal_stream_options_t options = LINUX_OPEN_FLAGS_TO_PAL_OPTIONS(flags);
    mode_t host_perm = HOST_PERM(perm);
    ret = DkStreamOpen(uri, access, host_perm, create, options, &palhdl);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }

    if (hdl) {
        hdl->uri = uri;
        uri = NULL;

        hdl->type = TYPE_CHROOT;
        hdl->pos = 0;
        hdl->pal_handle = palhdl;
    } else {
        DkObjectClose(palhdl);
    }
    ret = 0;

out:
    free(uri);
    return ret;
}

static int chroot_open(struct shim_handle* hdl, struct shim_dentry* dent, int flags) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    return chroot_do_open(hdl, dent, dent->inode->type, flags, /*perm=*/0);
}

static int chroot_creat(struct shim_handle* hdl, struct shim_dentry* dent, int flags, mode_t perm) {
    assert(locked(&g_dcache_lock));
    assert(!dent->inode);

    int ret;

    mode_t type = S_IFREG;

    ret = chroot_do_open(hdl, dent, type, flags | O_CREAT | O_EXCL, perm);
    if (ret < 0)
        return ret;

    return chroot_setup_dentry(dent, type, perm, /*size=*/0);
}

static int chroot_mkdir(struct shim_dentry* dent, mode_t perm) {
    assert(locked(&g_dcache_lock));
    assert(!dent->inode);

    int ret;

    mode_t type = S_IFDIR;

    ret = chroot_do_open(/*hdl=*/NULL, dent, type, O_CREAT | O_EXCL, perm);
    if (ret < 0)
        return ret;

    return chroot_setup_dentry(dent, type, perm, /*size=*/0);
}

static int chroot_flush(struct shim_handle* hdl) {
    assert(hdl->type == TYPE_CHROOT);

    int ret = DkStreamFlush(hdl->pal_handle);
    return pal_to_unix_errno(ret);
}

static ssize_t chroot_read(struct shim_handle* hdl, void* buf, size_t count) {
    assert(hdl->type == TYPE_CHROOT);

    ssize_t ret;

    if (count > SSIZE_MAX)
        return -EFBIG;

    struct shim_inode* inode = hdl->inode;
    lock(&hdl->lock);

    file_off_t pos = hdl->pos;

    /* Make sure we won't overflow `pos` */
    file_off_t max_end_pos;
    if (inode->type == S_IFREG && __builtin_add_overflow(pos, count, &max_end_pos)) {
        ret = -EFBIG;
        goto out;
    }

    size_t actual_count = count;
    ret = DkStreamRead(hdl->pal_handle, pos, &actual_count, buf, /*source=*/NULL, /*size=*/0);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }
    assert(actual_count <= count);
    if (inode->type == S_IFREG) {
        hdl->pos += actual_count;
    }
    ret = actual_count;

out:
    unlock(&hdl->lock);
    return ret;
}

static ssize_t chroot_write(struct shim_handle* hdl, const void* buf, size_t count) {
    assert(hdl->type == TYPE_CHROOT);

    ssize_t ret;

    if (count > SSIZE_MAX)
        return -EFBIG;

    struct shim_inode* inode = hdl->inode;
    lock(&inode->lock);
    lock(&hdl->lock);

    file_off_t pos = hdl->pos;

    /* Make sure we won't overflow `pos` */
    file_off_t max_end_pos;
    if (inode->type == S_IFREG && __builtin_add_overflow(pos, count, &max_end_pos)) {
        ret = -EFBIG;
        goto out;
    }

    size_t actual_count = count;
    ret = DkStreamWrite(hdl->pal_handle, pos, &actual_count, (void*)buf, /*dest=*/NULL);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }
    assert(count <= actual_count);
    if (inode->type == S_IFREG) {
        pos += actual_count;
        hdl->pos = pos;

        /* Update file size if we just wrote past the end of file */
        if (inode->size < pos)
            inode->size = pos;
    }
    ret = actual_count;

out:
    unlock(&hdl->lock);
    unlock(&inode->lock);
    return ret;
}

static int chroot_mmap(struct shim_handle* hdl, void** addr, size_t size, int prot, int flags,
                       uint64_t offset) {
    assert(hdl->type == TYPE_CHROOT);

    pal_prot_flags_t pal_prot = LINUX_PROT_TO_PAL(prot, flags);

    if (flags & MAP_ANONYMOUS)
        return -EINVAL;

    int ret = DkStreamMap(hdl->pal_handle, addr, pal_prot, offset, size);
    return pal_to_unix_errno(ret);
}

static int chroot_truncate(struct shim_handle* hdl, file_off_t size) {
    assert(hdl->type == TYPE_CHROOT);

    int ret;

    lock(&hdl->inode->lock);
    ret = DkStreamSetLength(hdl->pal_handle, size);
    if (ret == 0) {
        hdl->inode->size = size;
    } else {
        ret = pal_to_unix_errno(ret);
    }
    unlock(&hdl->inode->lock);
    return ret;
}

static int chroot_readdir(struct shim_dentry* dent, readdir_callback_t callback, void* arg) {
    int ret;
    PAL_HANDLE palhdl;
    char* buf = NULL;
    size_t buf_size = READDIR_BUF_SIZE;

    ret = chroot_temp_open(dent, S_IFDIR, &palhdl);
    if (ret < 0)
        return ret;

    buf = malloc(buf_size);
    if (!buf) {
        ret = -ENOMEM;
        goto out;
    }

    while (true) {
        size_t read_size = buf_size;
        ret = DkStreamRead(palhdl, /*offset=*/0, &read_size, buf, /*source=*/NULL, /*size=*/0);
        if (ret < 0) {
            ret = pal_to_unix_errno(ret);
            goto out;
        }
        if (read_size == 0) {
            /* End of directory listing */
            break;
        }

        /* Last entry must be null-terminated */
        assert(buf[read_size - 1] == '\0');

        /* Read all entries (separated by null bytes) and invoke `callback` on each */
        size_t start = 0;
        while (start < read_size - 1) {
            size_t end = start + strlen(&buf[start]);

            if (end == start) {
                log_error("chroot_readdir: empty name returned from PAL");
                BUG();
            }

            /* By the PAL convention, if a name ends with '/', it is a directory. However, we ignore
             * that distinction here and pass the name without '/' to the callback. */
            if (buf[end - 1] == '/')
                buf[end - 1] = '\0';

            if ((ret = callback(&buf[start], arg)) < 0)
                goto out;

            start = end + 1;
        }
    }
    ret = 0;

out:
    free(buf);
    DkObjectClose(palhdl);
    return ret;
}

static int chroot_unlink(struct shim_dentry* dent) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    int ret;

    PAL_HANDLE palhdl;
    ret = chroot_temp_open(dent, dent->inode->type, &palhdl);
    if (ret < 0)
        return ret;

    ret = DkStreamDelete(palhdl, PAL_DELETE_ALL);
    DkObjectClose(palhdl);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    return 0;
}

static int chroot_rename(struct shim_dentry* old, struct shim_dentry* new) {
    assert(locked(&g_dcache_lock));
    assert(old->inode);

    int ret;
    char* new_uri = NULL;

    ret = chroot_dentry_uri(new, old->inode->type, &new_uri);
    if (ret < 0)
        goto out;

    PAL_HANDLE palhdl;
    ret = chroot_temp_open(old, old->inode->type, &palhdl);
    if (ret < 0)
        goto out;

    ret = DkStreamChangeName(palhdl, new_uri);
    DkObjectClose(palhdl);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }
    ret = 0;

out:
    free(new_uri);
    return ret;
}

static int chroot_chmod(struct shim_dentry* dent, mode_t perm) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    int ret;

    lock(&dent->inode->lock);

    PAL_HANDLE palhdl;
    ret = chroot_temp_open(dent, dent->inode->type, &palhdl);
    if (ret < 0)
        goto out;

    mode_t host_perm = HOST_PERM(perm);
    PAL_STREAM_ATTR attr = {.share_flags = host_perm};
    ret = DkStreamAttributesSetByHandle(palhdl, &attr);
    DkObjectClose(palhdl);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }

    dent->inode->perm = perm;
    ret = 0;

out:
    unlock(&dent->inode->lock);
    return ret;
}

static int chroot_reopen(struct shim_handle* hdl, PAL_HANDLE* out_palhdl) {
    PAL_HANDLE palhdl;

    mode_t mode = 0;
    enum pal_access access = LINUX_OPEN_FLAGS_TO_PAL_ACCESS(hdl->flags);
    enum pal_create_mode create = PAL_CREATE_NEVER;
    pal_stream_options_t options = LINUX_OPEN_FLAGS_TO_PAL_OPTIONS(hdl->flags);
    int ret = DkStreamOpen(hdl->uri, access, mode, create, options, &palhdl);
    if (ret < 0)
        return pal_to_unix_errno(ret);
    *out_palhdl = palhdl;
    return 0;
}

/*
 * Prepare the handle to be sent to child process. If the corresponding file still exists on the
 * host, we will not checkpoint its PAL handle, but let the child process open another one.
 *
 * TODO: this is only necessary because PAL handles for protected files cannot be sent to child
 * process (`DkSendHandle`). This workaround limits the damage: inheriting a handle by child process
 * will now fail to work only if it's a handle for a protected file *and* the file has been deleted
 * from host.
 */
static int chroot_checkout(struct shim_handle* hdl) {
    assert(hdl->type == TYPE_CHROOT);
    assert(hdl->pal_handle);

    /* We should be holding `g_dcache_lock` for the whole checkpointing process. */
    assert(locked(&g_dcache_lock));

    /* We don't take `hdl->lock` because this is actually the handle *copied* for checkpointing (and
     * the lock isn't even properly initialized). */

    /* First, check if we have not deleted or renamed the file (the dentry contains the same
     * inode). */
    bool is_in_dentry = (hdl->dentry->inode == hdl->inode);

    if (is_in_dentry) {
        /* Then check if the file still exists on host. If so, we assume it can be opened by the
         * child process, so the PAL handle doesn't need sending. */
        PAL_STREAM_ATTR attr;
        if (DkStreamAttributesQuery(hdl->uri, &attr) == 0) {
            hdl->pal_handle = NULL;
        }
    }

    return 0;
}

static int chroot_checkin(struct shim_handle* hdl) {
    assert(hdl->type == TYPE_CHROOT);

    /* We don't take `hdl->lock` because this handle is being initialized (during checkpoint
     * restore). */

    if (!hdl->pal_handle) {
        PAL_HANDLE palhdl = NULL;
        int ret = chroot_reopen(hdl, &palhdl);
        if (ret < 0) {
            log_warning("%s: failed to open %s: %d", __func__, hdl->uri, ret);
            return ret;
        }
        assert(palhdl);
        hdl->pal_handle = palhdl;
    }
    return 0;
}

struct shim_fs_ops chroot_fs_ops = {
    .mount      = &chroot_mount,
    .flush      = &chroot_flush,
    .read       = &chroot_read,
    .write      = &chroot_write,
    .mmap       = &chroot_mmap,
    /* TODO: this function emulates lseek() completely inside the LibOS, but some device files may
     * report size == 0 during fstat() and may provide device-specific lseek() logic; this emulation
     * breaks for such device-specific cases */
    .seek       = &generic_inode_seek,
    .hstat      = &generic_inode_hstat,
    .truncate   = &chroot_truncate,
    .poll       = &generic_inode_poll,
    .checkout   = &chroot_checkout,
    .checkin    = &chroot_checkin,
};

struct shim_d_ops chroot_d_ops = {
    .open    = &chroot_open,
    .lookup  = &chroot_lookup,
    .creat   = &chroot_creat,
    .mkdir   = &chroot_mkdir,
    .stat    = &generic_inode_stat,
    .readdir = &chroot_readdir,
    .unlink  = &chroot_unlink,
    .rename  = &chroot_rename,
    .chmod   = &chroot_chmod,
};

struct shim_fs chroot_builtin_fs = {
    .name   = "chroot",
    .fs_ops = &chroot_fs_ops,
    .d_ops  = &chroot_d_ops,
};
