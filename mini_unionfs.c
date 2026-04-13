/*
 * Mini-UnionFS: A simplified Union File System using FUSE
 *
 * Features:
 *  - Layer stacking (lower read-only + upper read-write)
 *  - Copy-on-Write (CoW)
 *  - Whiteout file support for deletions
 *  - Full POSIX ops: getattr, readdir, read, write, create, unlink, mkdir, rmdir
 *  - EXTRA: rename, symlink, link, chmod, chown, truncate, utimens, statfs
 *  - EXTRA: multi-level directory CoW (recursive upper dir creation)
 *  - EXTRA: operation logging to a log file
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdarg.h>
#include <time.h>
#include <libgen.h>
#include <limits.h>

/* ─────────────────────────────────────────────
 * Global State
 * ───────────────────────────────────────────── */

struct unionfs_state {
    char *lower_dir;
    char *upper_dir;
    FILE *logfile;
};

#define STATE ((struct unionfs_state *)fuse_get_context()->private_data)

#define WHITEOUT_PREFIX ".wh."

/* ─────────────────────────────────────────────
 * Logging
 * ───────────────────────────────────────────── */

static void ufs_log(const char *fmt, ...) {
    struct unionfs_state *st = STATE;
    if (!st || !st->logfile) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(st->logfile, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(st->logfile, fmt, ap);
    va_end(ap);
    fprintf(st->logfile, "\n");
    fflush(st->logfile);
}

/* ─────────────────────────────────────────────
 * Path Helpers
 * ───────────────────────────────────────────── */

static void build_path(char *out, size_t sz, const char *base, const char *rel) {
    snprintf(out, sz, "%s%s", base, rel);
}

/* Build the whiteout path for a given file path.
 * e.g. /foo/bar.txt -> upper/.wh.bar.txt  (stored as upper/foo/.wh.bar.txt) */
static void build_whiteout_path(char *out, size_t sz,
                                const char *upper, const char *rel_path) {
    /* Split dirname and basename */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", rel_path);
    char *dir  = dirname(tmp);
    char tmp2[PATH_MAX];
    snprintf(tmp2, sizeof(tmp2), "%s", rel_path);
    char *base = basename(tmp2);

    if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0) {
        snprintf(out, sz, "%s/" WHITEOUT_PREFIX "%s", upper, base);
    } else {
        snprintf(out, sz, "%s%s/" WHITEOUT_PREFIX "%s", upper, dir, base);
    }
}

/* Check whether a whiteout marker exists for rel_path in upper */
static int is_whiteout(const char *rel_path) {
    char wh[PATH_MAX];
    build_whiteout_path(wh, sizeof(wh), STATE->upper_dir, rel_path);
    return access(wh, F_OK) == 0;
}

typedef enum {
    LAYER_UPPER,
    LAYER_LOWER,
    LAYER_NONE
} layer_t;

/*
 * resolve_path:
 *   Fills `resolved` with the real filesystem path for `rel_path`.
 *   Returns the layer it came from (LAYER_UPPER / LAYER_LOWER / LAYER_NONE).
 *   If a whiteout is found, returns LAYER_NONE.
 */
static layer_t resolve_path(const char *rel_path, char *resolved, size_t sz) {
    /* 1. Whiteout check */
    if (is_whiteout(rel_path)) {
        ufs_log("resolve: %s -> WHITEOUT", rel_path);
        return LAYER_NONE;
    }

    /* 2. Upper layer */
    char upper[PATH_MAX];
    build_path(upper, sizeof(upper), STATE->upper_dir, rel_path);
    if (access(upper, F_OK) == 0) {
        if (resolved) snprintf(resolved, sz, "%s", upper);
        ufs_log("resolve: %s -> UPPER (%s)", rel_path, upper);
        return LAYER_UPPER;
    }

    /* 3. Lower layer */
    char lower[PATH_MAX];
    build_path(lower, sizeof(lower), STATE->lower_dir, rel_path);
    if (access(lower, F_OK) == 0) {
        if (resolved) snprintf(resolved, sz, "%s", lower);
        ufs_log("resolve: %s -> LOWER (%s)", rel_path, lower);
        return LAYER_LOWER;
    }

    return LAYER_NONE;
}

/* ─────────────────────────────────────────────
 * Copy-on-Write helpers
 * ───────────────────────────────────────────── */

/* Recursively create directories in upper mirroring lower's path */
static int ensure_upper_dir(const char *rel_dir_path) {
    if (strcmp(rel_dir_path, "/") == 0 || strlen(rel_dir_path) == 0)
        return 0;

    char upper[PATH_MAX];
    build_path(upper, sizeof(upper), STATE->upper_dir, rel_dir_path);

    struct stat st;
    if (stat(upper, &st) == 0) return 0; /* already exists */

    /* Make parent first */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", rel_dir_path);
    char *parent = dirname(tmp);
    int r = ensure_upper_dir(parent);
    if (r != 0) return r;

    /* Stat the corresponding lower dir to copy permissions */
    char lower[PATH_MAX];
    build_path(lower, sizeof(lower), STATE->lower_dir, rel_dir_path);
    mode_t mode = 0755;
    if (stat(lower, &st) == 0) mode = st.st_mode & 07777;

    if (mkdir(upper, mode) != 0 && errno != EEXIST)
        return -errno;
    return 0;
}

/* Copy a file from src (real path) to dst (real path) */
static int copy_file(const char *src, const char *dst) {
    int fd_src = open(src, O_RDONLY);
    if (fd_src < 0) return -errno;

    struct stat st;
    fstat(fd_src, &st);

    int fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (fd_dst < 0) { close(fd_src); return -errno; }

    char buf[65536];
    ssize_t n;
    while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
        if (write(fd_dst, buf, n) != n) {
            close(fd_src); close(fd_dst); return -EIO;
        }
    }
    close(fd_src);
    close(fd_dst);
    return (n < 0) ? -errno : 0;
}

/*
 * cow_promote: promote a lower-layer file to upper so we can write it.
 * Creates all intermediate upper directories as needed.
 */
static int cow_promote(const char *rel_path) {
    char lower[PATH_MAX], upper[PATH_MAX];
    build_path(lower, sizeof(lower), STATE->lower_dir, rel_path);
    build_path(upper, sizeof(upper), STATE->upper_dir, rel_path);

    /* Ensure parent dirs exist in upper */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", rel_path);
    char *dir = dirname(tmp);
    int r = ensure_upper_dir(dir);
    if (r != 0) return r;

    r = copy_file(lower, upper);
    if (r == 0)
        ufs_log("CoW: promoted %s -> %s", lower, upper);
    return r;
}

/* ─────────────────────────────────────────────
 * FUSE Callbacks
 * ───────────────────────────────────────────── */

static int ufs_getattr(const char *path, struct stat *stbuf,
                       struct fuse_file_info *fi) {
    (void)fi;
    ufs_log("getattr: %s", path);
    memset(stbuf, 0, sizeof(*stbuf));

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    if (lstat(resolved, stbuf) != 0) return -errno;
    return 0;
}

/* ── seen-set helpers (replaces GNU nested functions) ── */

static int seen_contains(char **seen, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(seen[i], name) == 0) return 1;
    return 0;
}

static int seen_add(char ***seen, int *n, const char *name) {
    char **tmp = realloc(*seen, (*n + 1) * sizeof(char *));
    if (!tmp) return -1;
    *seen = tmp;
    (*seen)[*n] = strdup(name);
    if (!(*seen)[*n]) return -1;
    (*n)++;
    return 0;
}

static void seen_free(char **seen, int n) {
    for (int i = 0; i < n; i++) free(seen[i]);
    free(seen);
}

static int ufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    ufs_log("readdir: %s", path);

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    char **seen = NULL;
    int seen_n  = 0;

    /* ── Iterate upper directory ── */
    char upper_dir_path[PATH_MAX];
    build_path(upper_dir_path, sizeof(upper_dir_path), STATE->upper_dir, path);

    DIR *dp = opendir(upper_dir_path);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            /* Hide whiteout marker files themselves */
            if (strncmp(de->d_name, WHITEOUT_PREFIX, strlen(WHITEOUT_PREFIX)) == 0)
                continue;
            if (!seen_contains(seen, seen_n, de->d_name)) {
                filler(buf, de->d_name, NULL, 0, 0);
                seen_add(&seen, &seen_n, de->d_name);
            }
        }
        closedir(dp);
    }

    /* ── Iterate lower directory — skip whiteout'd and already-seen entries ── */
    char lower_dir_path[PATH_MAX];
    build_path(lower_dir_path, sizeof(lower_dir_path), STATE->lower_dir, path);

    dp = opendir(lower_dir_path);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (seen_contains(seen, seen_n, de->d_name)) continue;

            /* Build full relative path for whiteout check */
            char rel[PATH_MAX];
            if (strcmp(path, "/") == 0)
                snprintf(rel, sizeof(rel), "/%s", de->d_name);
            else
                snprintf(rel, sizeof(rel), "%s/%s", path, de->d_name);

            if (is_whiteout(rel)) continue;

            filler(buf, de->d_name, NULL, 0, 0);
            seen_add(&seen, &seen_n, de->d_name);
        }
        closedir(dp);
    }

    seen_free(seen, seen_n);
    return 0;
}

static int ufs_open(const char *path, struct fuse_file_info *fi) {
    ufs_log("open: %s flags=0x%x", path, fi->flags);

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    /* If opening for write and file is in lower, do CoW */
    if ((fi->flags & O_ACCMODE) != O_RDONLY && layer == LAYER_LOWER) {
        int r = cow_promote(path);
        if (r != 0) return r;
        /* Update resolved to upper path */
        build_path(resolved, sizeof(resolved), STATE->upper_dir, path);
    }

    int fd = open(resolved, fi->flags);
    if (fd < 0) return -errno;
    fi->fh = fd;
    return 0;
}

static int ufs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    ufs_log("read: %s size=%zu off=%ld", path, size, offset);
    int res = pread(fi->fh, buf, size, offset);
    if (res < 0) return -errno;
    return res;
}

static int ufs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi) {
    ufs_log("write: %s size=%zu off=%ld", path, size, offset);
    int res = pwrite(fi->fh, buf, size, offset);
    if (res < 0) return -errno;
    return res;
}

static int ufs_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    close(fi->fh);
    return 0;
}

static int ufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    ufs_log("create: %s mode=%o", path, mode);

    /* Ensure parent dir exists in upper */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *dir = dirname(tmp);
    ensure_upper_dir(dir);

    /* Remove any stale whiteout for this name */
    char wh[PATH_MAX];
    build_whiteout_path(wh, sizeof(wh), STATE->upper_dir, path);
    unlink(wh);

    char upper[PATH_MAX];
    build_path(upper, sizeof(upper), STATE->upper_dir, path);
    int fd = open(upper, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0) return -errno;
    fi->fh = fd;
    return 0;
}

static int ufs_unlink(const char *path) {
    ufs_log("unlink: %s", path);

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    if (layer == LAYER_UPPER) {
        /* Direct delete */
        if (unlink(resolved) != 0) return -errno;
        /* If a lower copy also exists, create whiteout so it stays hidden */
        char lower[PATH_MAX];
        build_path(lower, sizeof(lower), STATE->lower_dir, path);
        if (access(lower, F_OK) == 0) {
            char wh[PATH_MAX];
            build_whiteout_path(wh, sizeof(wh), STATE->upper_dir, path);
            int fd = open(wh, O_CREAT | O_WRONLY, 0000);
            if (fd >= 0) close(fd);
        }
    } else {
        /* Lower-only file: create whiteout */
        char wh[PATH_MAX];
        build_whiteout_path(wh, sizeof(wh), STATE->upper_dir, path);

        /* Ensure upper parent dir exists */
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", path);
        ensure_upper_dir(dirname(tmp));

        int fd = open(wh, O_CREAT | O_WRONLY, 0000);
        if (fd < 0) return -errno;
        close(fd);
        ufs_log("whiteout created: %s", wh);
    }
    return 0;
}

static int ufs_mkdir(const char *path, mode_t mode) {
    ufs_log("mkdir: %s mode=%o", path, mode);

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    ensure_upper_dir(dirname(tmp));

    /* Remove whiteout if any */
    char wh[PATH_MAX];
    build_whiteout_path(wh, sizeof(wh), STATE->upper_dir, path);
    unlink(wh);

    char upper[PATH_MAX];
    build_path(upper, sizeof(upper), STATE->upper_dir, path);
    if (mkdir(upper, mode) != 0) return -errno;
    return 0;
}

static int ufs_rmdir(const char *path) {
    ufs_log("rmdir: %s", path);

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    if (layer == LAYER_UPPER) {
        if (rmdir(resolved) != 0) return -errno;
        /* Whiteout if lower also has it */
        char lower[PATH_MAX];
        build_path(lower, sizeof(lower), STATE->lower_dir, path);
        if (access(lower, F_OK) == 0) {
            char wh[PATH_MAX];
            build_whiteout_path(wh, sizeof(wh), STATE->upper_dir, path);
            int fd = open(wh, O_CREAT | O_WRONLY, 0000);
            if (fd >= 0) close(fd);
        }
    } else {
        /* Lower-only dir: create whiteout */
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", path);
        ensure_upper_dir(dirname(tmp));

        char wh[PATH_MAX];
        build_whiteout_path(wh, sizeof(wh), STATE->upper_dir, path);
        int fd = open(wh, O_CREAT | O_WRONLY, 0000);
        if (fd < 0) return -errno;
        close(fd);
    }
    return 0;
}

static int ufs_rename(const char *from, const char *to, unsigned int flags) {
    ufs_log("rename: %s -> %s", from, to);
    (void)flags;

    char resolved_from[PATH_MAX];
    layer_t layer = resolve_path(from, resolved_from, sizeof(resolved_from));
    if (layer == LAYER_NONE) return -ENOENT;

    /* CoW if source is in lower */
    if (layer == LAYER_LOWER) {
        int r = cow_promote(from);
        if (r != 0) return r;
        build_path(resolved_from, sizeof(resolved_from), STATE->upper_dir, from);
    }

    /* Ensure dest upper parent exists */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", to);
    ensure_upper_dir(dirname(tmp));

    char upper_to[PATH_MAX];
    build_path(upper_to, sizeof(upper_to), STATE->upper_dir, to);

    if (rename(resolved_from, upper_to) != 0) return -errno;

    /* Whiteout old location if it had a lower copy */
    char lower_from[PATH_MAX];
    build_path(lower_from, sizeof(lower_from), STATE->lower_dir, from);
    if (access(lower_from, F_OK) == 0) {
        char wh[PATH_MAX];
        build_whiteout_path(wh, sizeof(wh), STATE->upper_dir, from);
        int fd = open(wh, O_CREAT | O_WRONLY, 0000);
        if (fd >= 0) close(fd);
    }
    return 0;
}

static int ufs_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi) {
    ufs_log("truncate: %s size=%ld", path, size);
    (void)fi;

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    if (layer == LAYER_LOWER) {
        int r = cow_promote(path);
        if (r != 0) return r;
        build_path(resolved, sizeof(resolved), STATE->upper_dir, path);
    }
    if (truncate(resolved, size) != 0) return -errno;
    return 0;
}

static int ufs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    ufs_log("chmod: %s mode=%o", path, mode);
    (void)fi;

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    if (layer == LAYER_LOWER) {
        int r = cow_promote(path);
        if (r != 0) return r;
        build_path(resolved, sizeof(resolved), STATE->upper_dir, path);
    }
    if (chmod(resolved, mode) != 0) return -errno;
    return 0;
}

static int ufs_chown(const char *path, uid_t uid, gid_t gid,
                     struct fuse_file_info *fi) {
    ufs_log("chown: %s uid=%d gid=%d", path, uid, gid);
    (void)fi;

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    if (layer == LAYER_LOWER) {
        int r = cow_promote(path);
        if (r != 0) return r;
        build_path(resolved, sizeof(resolved), STATE->upper_dir, path);
    }
    if (lchown(resolved, uid, gid) != 0) return -errno;
    return 0;
}

static int ufs_utimens(const char *path, const struct timespec tv[2],
                       struct fuse_file_info *fi) {
    (void)fi;
    ufs_log("utimens: %s", path);

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    if (layer == LAYER_LOWER) {
        int r = cow_promote(path);
        if (r != 0) return r;
        build_path(resolved, sizeof(resolved), STATE->upper_dir, path);
    }
    if (utimensat(AT_FDCWD, resolved, tv, AT_SYMLINK_NOFOLLOW) != 0)
        return -errno;
    return 0;
}

static int ufs_symlink(const char *target, const char *linkpath) {
    ufs_log("symlink: %s -> %s", linkpath, target);

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", linkpath);
    ensure_upper_dir(dirname(tmp));

    char upper[PATH_MAX];
    build_path(upper, sizeof(upper), STATE->upper_dir, linkpath);
    if (symlink(target, upper) != 0) return -errno;
    return 0;
}

static int ufs_readlink(const char *path, char *buf, size_t size) {
    ufs_log("readlink: %s", path);

    char resolved[PATH_MAX];
    layer_t layer = resolve_path(path, resolved, sizeof(resolved));
    if (layer == LAYER_NONE) return -ENOENT;

    ssize_t r = readlink(resolved, buf, size - 1);
    if (r < 0) return -errno;
    buf[r] = '\0';
    return 0;
}

static int ufs_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    ufs_log("statfs");
    char upper[PATH_MAX];
    build_path(upper, sizeof(upper), STATE->upper_dir, "/");
    if (statvfs(upper, stbuf) != 0) return -errno;
    return 0;
}

/* ─────────────────────────────────────────────
 * Operations Table
 * ───────────────────────────────────────────── */

static struct fuse_operations unionfs_ops = {
    .getattr  = ufs_getattr,
    .readdir  = ufs_readdir,
    .open     = ufs_open,
    .read     = ufs_read,
    .write    = ufs_write,
    .release  = ufs_release,
    .create   = ufs_create,
    .unlink   = ufs_unlink,
    .mkdir    = ufs_mkdir,
    .rmdir    = ufs_rmdir,
    .rename   = ufs_rename,
    .truncate = ufs_truncate,
    .chmod    = ufs_chmod,
    .chown    = ufs_chown,
    .utimens  = ufs_utimens,
    .symlink  = ufs_symlink,
    .readlink = ufs_readlink,
    .statfs   = ufs_statfs,
};

/* ─────────────────────────────────────────────
 * main()
 * ───────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "Mini-UnionFS: A FUSE-based union filesystem\n\n"
        "Usage: %s <lower_dir> <upper_dir> <mount_point> [FUSE options]\n\n"
        "  lower_dir   - Read-only base layer (image layer)\n"
        "  upper_dir   - Read-write overlay layer (container layer)\n"
        "  mount_point - Where the unified view is mounted\n\n"
        "FUSE options: -d (debug), -f (foreground), -s (single-threaded)\n"
        "Example:\n"
        "  %s ./lower ./upper ./mnt -f\n", prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    struct unionfs_state *state = calloc(1, sizeof(*state));
    if (!state) { perror("calloc"); return 1; }

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        fprintf(stderr, "Error: lower_dir and upper_dir must exist.\n");
        free(state);
        return 1;
    }

    /* Open log file in upper_dir */
    char logpath[PATH_MAX];
    snprintf(logpath, sizeof(logpath), "%s/.unionfs.log", state->upper_dir);
    state->logfile = fopen(logpath, "a");

    fprintf(stderr, "Mini-UnionFS starting...\n");
    fprintf(stderr, "  Lower (read-only): %s\n", state->lower_dir);
    fprintf(stderr, "  Upper (read-write): %s\n", state->upper_dir);
    fprintf(stderr, "  Mount point: %s\n", argv[3]);
    fprintf(stderr, "  Log file: %s\n", logpath);

    /* Shift args so fuse_main sees: prog mountpoint [fuse-opts] */
    /* Build new argv: [0]=prog [1]=mountpoint [2..]=fuse opts */
    int new_argc = argc - 2; /* remove lower_dir and upper_dir */
    char **new_argv = malloc(new_argc * sizeof(char *));
    new_argv[0] = argv[0];
    new_argv[1] = argv[3]; /* mount point */
    for (int i = 4; i < argc; i++)
        new_argv[i - 2] = argv[i];

    int ret = fuse_main(new_argc, new_argv, &unionfs_ops, state);

    free(new_argv);
    if (state->logfile) fclose(state->logfile);
    free(state->lower_dir);
    free(state->upper_dir);
    free(state);
    return ret;
}
