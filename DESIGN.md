# Mini-UnionFS — Design Document

## 1. Overview

Mini-UnionFS is a userspace union filesystem built on FUSE (Filesystem in Userspace). It presents a single merged view of two directories:

- **lower_dir** — read-only base layer (analogous to a Docker image layer)
- **upper_dir** — read-write overlay layer (analogous to a Docker container layer)

This mimics the overlay filesystem mechanism used by container runtimes like Docker (via `overlayfs`), but runs entirely in userspace, making it portable and debuggable without kernel modules.

---

## 2. Architecture

```
User Process
    │  open("/mnt/file.txt")
    ▼
Linux VFS
    │  FUSE device (/dev/fuse)
    ▼
mini_unionfs (userspace daemon)
    │
    ├─ resolve_path("/file.txt")
    │       ├─ Check: upper/.wh.file.txt  → ENOENT (whiteout)
    │       ├─ Check: upper/file.txt      → LAYER_UPPER
    │       └─ Check: lower/file.txt      → LAYER_LOWER
    │
    ├─ CoW engine  (cow_promote)
    └─ Whiteout engine (ufs_unlink)
```

FUSE routes every VFS call (open, read, write, mkdir, …) to our registered `fuse_operations` callbacks. Our daemon then dispatches each call to the real lower or upper directory.

---

## 3. Core Data Structures

### 3.1 `unionfs_state`

Global context stored in FUSE's private data pointer, accessible from every callback via `fuse_get_context()->private_data`.

```c
struct unionfs_state {
    char *lower_dir;  // realpath of the read-only base layer
    char *upper_dir;  // realpath of the read-write overlay layer
    FILE *logfile;    // optional operation log
};
```

### 3.2 `layer_t` enum

Returned by `resolve_path()` to identify where a file was found:

```c
typedef enum {
    LAYER_UPPER,  // file exists in upper_dir (possibly promoted by CoW)
    LAYER_LOWER,  // file exists only in lower_dir
    LAYER_NONE    // file doesn't exist or is whiteout'd
} layer_t;
```

---

## 4. Key Algorithms

### 4.1 Path Resolution (`resolve_path`)

The core lookup function. Given a virtual path `/foo/bar.txt`, it checks in order:

1. **Whiteout check**: Does `upper_dir/foo/.wh.bar.txt` exist? → return `LAYER_NONE` (file is deleted)
2. **Upper layer**: Does `upper_dir/foo/bar.txt` exist? → return `LAYER_UPPER`
3. **Lower layer**: Does `lower_dir/foo/bar.txt` exist? → return `LAYER_LOWER`
4. Otherwise → return `LAYER_NONE`

This order guarantees upper always wins over lower, and deletions (whiteouts) shadow both.

### 4.2 Copy-on-Write (`cow_promote`)

Triggered in `ufs_open()` when a write-mode open targets a `LAYER_LOWER` file, and also in `ufs_truncate()`, `ufs_chmod()`, `ufs_chown()`, `ufs_utimens()` for the same reason.

Steps:
1. Call `ensure_upper_dir()` to recursively create the file's parent directory tree in `upper_dir`, mirroring the lower's structure.
2. Call `copy_file(lower_path, upper_path)` — a buffered 64 KB block copy that preserves file permissions from `fstat`.
3. From this point, all mutations happen in `upper_dir`. The `lower_dir` file is never modified.

### 4.3 Whiteout Mechanism (`ufs_unlink`, `ufs_rmdir`)

When a file/directory from `lower_dir` is deleted:

1. No physical deletion of the lower file (it is read-only).
2. Create a zero-byte marker file: `upper_dir/<parent>/.wh.<filename>`.
3. `resolve_path()` checks for this marker *before* checking either layer, so the file becomes invisible.

When a file from `upper_dir` is deleted:
1. Physical `unlink()` removes it from upper.
2. If a same-named file also exists in lower, a whiteout is still created to prevent the lower copy from re-appearing.

### 4.4 Directory Merging (`ufs_readdir`)

`readdir` must produce a unified listing with no duplicates and no whiteout'd names:

1. Read all entries from `upper_dir/path`, skip entries starting with `.wh.`, add to a `seen` set.
2. Read all entries from `lower_dir/path`, skip any name already in `seen`, skip any name that is whiteout'd, then add remaining entries.
3. The `seen` set is a dynamic `char**` array, linearly scanned (sufficient for typical directory sizes).

---

## 5. Edge Cases & Handling

| Scenario | Handling |
|---|---|
| Write to a lower file whose upper parent dir doesn't exist | `ensure_upper_dir()` recursively creates all missing dirs before CoW copy |
| Delete a file in upper that has a lower shadow | Physical unlink + create whiteout so lower stays hidden |
| Create a file with same name as a former whiteout | `ufs_create()` calls `unlink(wh_path)` before creating the new file |
| Rename a lower file | CoW promote source → rename in upper → create whiteout at old location |
| `readdir` on a path with whiteouts in a subdirectory | Per-entry whiteout check with full relative path |
| Nested subdirectory in lower modified via mount | `ensure_upper_dir` walks dirname chain recursively before CoW |
| FUSE args overlap with our custom args | main() shifts argv: strips lower/upper, passes mount + FUSE opts to `fuse_main` |
| Symlinks | `ufs_symlink` creates in upper; `ufs_readlink` resolves via `resolve_path` |

---

## 6. Extra Features (Beyond Requirements)

| Feature | Implementation |
|---|---|
| `rename` | CoW + rename in upper + whiteout old path |
| `truncate` | CoW then `truncate(2)` in upper |
| `chmod` / `chown` | CoW then `chmod`/`lchown` in upper |
| `utimens` | CoW then `utimensat` in upper |
| `symlink` / `readlink` | Full symlink support via upper layer |
| `statfs` | Reports upper layer's disk stats |
| Operation log | Every FUSE call logged to `upper_dir/.unionfs.log` with timestamps |
| Multi-level CoW | `ensure_upper_dir` recursively mirrors lower's dir tree in upper |

---

## 7. Build & Run

```bash
# Install dependencies (once)
sudo apt-get install -y fuse3 libfuse3-dev pkg-config

# Build
make

# Run (foreground, single-threaded — good for development)
mkdir -p lower upper mnt
./mini_unionfs ./lower ./upper ./mnt -f -s

# Run in background
./mini_unionfs ./lower ./upper ./mnt -s

# Unmount
fusermount3 -u ./mnt

# Run tests
chmod +x test_unionfs.sh
./test_unionfs.sh
```

---

## 8. Limitations

- **No hardlink support**: `link(2)` is not implemented; hardlinks across layers are complex in overlayfs semantics.
- **No `mmap` write support**: Memory-mapped write would require `fuse_file_info.direct_io` and per-fd state, omitted for simplicity.
- **No concurrent CoW safety**: Multiple simultaneous writers on the same file could race during promotion. Production overlayfs uses inode locks; this implementation is single-threaded (`-s` flag recommended).
- **`readdir` seen-set is O(n²)**: Linear scan is fine for ≤1000 entries per directory; a hash set would be needed for very large directories.
