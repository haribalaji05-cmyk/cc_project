# Mini-UnionFS

Mini-UnionFS is a userspace union filesystem built with FUSE. It merges a read-only lower directory and a read-write upper directory into a single mounted view.

This project demonstrates overlay-style behavior with Copy-on-Write (CoW) and whiteout-based deletions.

## Overview

The filesystem presents three directories:

- `lower/`: base layer (read-only source)
- `upper/`: writable layer (all mutations happen here)
- `mnt/`: merged mount point exposed to applications

At lookup time, Mini-UnionFS resolves paths in this order:

1. Whiteout marker in upper (`.wh.<name>`) -> treat as deleted
2. File or directory in upper
3. File or directory in lower

## Key Features

- Layered filesystem view: lower + upper merged into one mount
- Copy-on-Write: writing to lower-only files promotes them to upper first
- Whiteout deletion: deleting lower-only files creates `.wh.*` markers in upper
- Core POSIX-style operations via FUSE callbacks
- Metadata operations with CoW safety (`chmod`, `chown`, `truncate`, `utimens`)
- Symlink support (`symlink`, `readlink`)
- `statfs` support using upper-layer storage stats
- CoW-aware rename behavior
- Per-operation logging to `upper/.unionfs.log`

## Requirements

- Linux or WSL2
- `fuse3`
- `libfuse3-dev`
- `gcc`
- `make`
- `pkg-config`

## Build

```bash
make
```

## Basic Usage

```bash
mkdir -p lower upper mnt

echo "hello" > lower/hello.txt

# Start filesystem (single-threaded recommended)
./mini_unionfs ./lower ./upper ./mnt -s
```

In another terminal:

```bash
ls mnt
cat mnt/hello.txt
echo "updated" >> mnt/hello.txt
rm mnt/hello.txt
```

Unmount when done:

```bash
fusermount3 -u mnt
```

## Command-Line Flags

- `-f`: run in foreground
- `-s`: single-threaded mode
- `-d`: FUSE debug output

## Test Script

```bash
./test_unionfs.sh
```

## Repository Layout

```text
.
├── mini_unionfs.c
├── Makefile
├── test_unionfs.sh
├── setup_wsl.sh
├── DESIGN.md
└── README.md
```

## Limitations

- Hardlinks are not implemented
- No explicit mmap-write handling
- No fine-grained locking for concurrent CoW races

## License

MIT
