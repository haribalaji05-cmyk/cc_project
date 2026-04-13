# Mini-UnionFS

A userspace Union File System built with [FUSE](https://github.com/libfuse/libfuse), modeled after the overlay filesystem mechanism that powers Docker containers.

It stacks a **read-only lower layer** (base image) on top of a **read-write upper layer** (container layer) and presents them as a single unified mount — with full **Copy-on-Write** and **whiteout-based deletion** semantics.

```
lower/ (read-only)      upper/ (read-write)
  ├── base.txt            ├── modified.txt   ← CoW promoted copy
  ├── secret.txt          └── .wh.secret.txt ← whiteout (deletion marker)
  └── config.txt
            │                     │
            └────────┬────────────┘
                     ▼
                   mnt/  (unified view)
                     ├── base.txt
                     ├── modified.txt
                     └── config.txt        ← secret.txt is hidden
```

---

## Features

| Feature | Details |
|---|---|
| **Layer stacking** | Lower (read-only) + Upper (read-write) merged into one mount point |
| **Copy-on-Write** | Writing to a lower-layer file promotes it to upper; lower is never touched |
| **Whiteout deletions** | Deleting a lower file creates `upper/.wh.<name>` instead of touching lower |
| **Full POSIX ops** | `getattr`, `readdir`, `read`, `write`, `create`, `unlink`, `mkdir`, `rmdir` |
| **Rename** | CoW-aware rename with whiteout at old path |
| **chmod / chown / truncate / utimens** | All trigger CoW before mutating metadata |
| **Symlinks** | `symlink` and `readlink` fully supported |
| **statfs** | Reports upper layer disk stats |
| **Multi-level CoW** | Recursively mirrors lower directory trees in upper before promoting |
| **Operation log** | Every FUSE call timestamped to `upper/.unionfs.log` |

---

## Requirements

- Linux (or WSL2 on Windows)
- `fuse3` + `libfuse3-dev`
- `gcc`, `make`, `pkg-config`

---

## Installation

```bash
# 1. Install dependencies
sudo apt-get update
sudo apt-get install -y fuse3 libfuse3-dev pkg-config gcc make

# 2. Clone and build
git clone https://github.com/<your-username>/mini-unionfs.git
cd mini-unionfs
make
```

---

## Usage

```bash
# Create your layer directories and mount point
mkdir -p lower upper mnt

# Put files in the base (lower) layer
echo "hello from base image" > lower/hello.txt
echo "sensitive config"      > lower/config.txt

# Mount the union filesystem
./mini_unionfs ./lower ./upper ./mnt -s

# In another terminal — interact with the unified view
ls mnt/                          # sees both layers merged
cat mnt/hello.txt                # reads from lower

echo "overridden!" >> mnt/hello.txt   # triggers Copy-on-Write
cat upper/hello.txt              # promoted copy is here
cat lower/hello.txt              # lower is untouched

rm mnt/config.txt                # creates upper/.wh.config.txt
ls mnt/                          # config.txt is gone from the view
ls -la upper/                    # .wh.config.txt whiteout marker visible
cat lower/config.txt             # original still safe in lower

# Unmount when done
fusermount3 -u mnt
```

### Flags

| Flag | Effect |
|---|---|
| `-f` | Run in foreground (shows debug output, Ctrl+C to stop) |
| `-s` | Single-threaded mode (recommended — no locking implemented) |
| `-d` | FUSE debug mode (very verbose) |

---

## WSL2 Setup

FUSE requires the `/dev/fuse` device to be available. On WSL2:

```bash
# Enable FUSE kernel module
sudo modprobe fuse

# Verify
ls /dev/fuse   # should exist

# If /dev/fuse is missing, update WSL from PowerShell:
# wsl --update
# then restart WSL
```

A convenience script is included that does the full one-time setup and runs a smoke test:

```bash
chmod +x setup_wsl.sh
./setup_wsl.sh
```

---

## Running Tests

```bash
chmod +x test_unionfs.sh
./test_unionfs.sh
```

The test suite covers all core and extra features:

```
━━━ Core: Layer Visibility ━━━
Test 1a: Lower-layer file visible...              PASSED
Test 1b: Upper overrides lower for same filename... PASSED
Test 1c: Subdirectory from lower is visible...    PASSED
Test 1d: Nested file in lower subdir visible...   PASSED
━━━ Core: Copy-on-Write ━━━
Test 2a: Append to lower-layer file triggers CoW... PASSED
Test 2b: Lower file is unmodified after CoW...    PASSED
Test 2c: CoW file still readable via mount...     PASSED
━━━ Core: Whiteout (Deletion) ━━━
Test 3a: Deleting lower-layer file hides it...    PASSED
Test 3b: Whiteout marker created in upper...      PASSED
Test 3c: Original lower file untouched...         PASSED
━━━ Extra: Create / Rename / Truncate / Stat ━━━
...                                               PASSED (10 more)

Results: 20 passed / 0 failed / 20 total
```

---

## How It Works

### Path Resolution

Every FUSE callback calls `resolve_path()` which checks in order:

1. **Whiteout?** — does `upper/.wh.<name>` exist? → `ENOENT`
2. **Upper layer** — does `upper/<path>` exist? → use it
3. **Lower layer** — does `lower/<path>` exist? → use it (read-only)
4. → `ENOENT`

### Copy-on-Write

```
User writes to mnt/file.txt (exists only in lower)
        │
        ▼
ensure_upper_dir()   ← recursively mirrors parent dirs in upper
        │
        ▼
copy_file(lower/file.txt → upper/file.txt)   ← preserves permissions
        │
        ▼
write applied to upper/file.txt   ← lower never touched
```

### Whiteout

```
User deletes mnt/config.txt (exists only in lower)
        │
        ▼
create upper/.wh.config.txt   ← zero-byte marker
        │
resolve_path() checks this first on every future lookup
        │
        ▼
config.txt invisible in mount   ← lower/config.txt physically safe
```

---

## Project Structure

```
.
├── mini_unionfs.c   # Full FUSE implementation (~700 lines, C99)
├── Makefile         # Build with pkg-config for fuse3
├── test_unionfs.sh  # Automated test suite (20 tests)
├── setup_wsl.sh     # WSL2 one-time setup + smoke test
├── DESIGN.md        # Design document (data structures, edge cases)
└── README.md
```

---

## Known Limitations

- **No hardlinks** — `link(2)` is not implemented; cross-layer hardlinks require inode-level tracking.
- **No `mmap` write support** — memory-mapped writes bypass the FUSE `write` callback.
- **No concurrent write safety** — run with `-s` (single-threaded). Concurrent CoW promotion of the same file could race without inode locking.
- **Linear seen-set in `readdir`** — O(n²) scan; fine for typical directory sizes, a hash set would be needed for >10k entries per directory.

---

## References

- [Linux `overlayfs` documentation](https://www.kernel.org/doc/html/latest/filesystems/overlayfs.html)
- [libfuse — FUSE userspace library](https://github.com/libfuse/libfuse)
- [Docker storage drivers](https://docs.docker.com/storage/storagedriver/)

---

## License

MIT
