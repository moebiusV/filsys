# 1. Context and motivation

## 1.1 What libfilsys already is

libfilsys is a version-agnostic access layer for Research Unix filesystem
images: PDP-7 through Tenth Edition, plus 32V, Coherent, early Xenix,
2.9/2.11BSD, System III and System V. One engine handles 16-, 18-, 32- and
64-bit word/block addressing in big-, little- and middle-endian order. It ships
three userspace tools — `mount.filsys` (FUSE), `mkfs.filsys`, `fsck.filsys`,
`findfs.filsys` — plus a FUSE-free core (`test_matrix` + the mkfs/fsck
subprocesses) that runs `make check` with no FUSE installed.

The engine's internal shape (this is what makes the kernel port tractable):

- **All raw I/O is already behind a vtable.** `filsys_io_t` is exactly two
  function pointers (`filsys_common.h:43`):

  ```c
  typedef struct filsys_io {
      int (*read)(struct filsys_edition *fs, void *buf, size_t n, off_t off);
      int (*write)(struct filsys_edition *fs, const void *buf, size_t n, off_t off);
  } filsys_io_t;
  ```

  The default `filsys_io_file` is `pread`/`pwrite` on `fs->fd` (`v7fs.c:245`).
  `v7fs_read_block`/`v7fs_write_block` compute the byte offset and call
  `fs->io->read/write`. Every backend routes its block access this way, so a
  kernel transport swaps in with **zero codec changes**.

- **Per-mount state is a plain C struct.** `filsys_edition_t`
  (`filsys_common.h:158`) holds `fd`, `readonly`, `io`, `base`, `imgsize`, the
  in-core superblock, and a `union` of free-list/bitmap allocator state. It is
  self-contained — no globals in the format engine.

- **The mutation ops decompose to inode-anchored primitives.** The public
  path-based API (`filsys_create`, `filsys_unlink`, `filsys_rename`, …) is a
  thin layer over `dir_lookup` / `dir_add` / `dir_remove` (the `filsys_dir_ops`
  vtable), `ialloc` / `ifree`, `bmap`, and `filsys_read_inode` /
  `filsys_write_inode`. The kernel driver needs those primitives, not path
  strings.

- **Inode-based read/write already exist** as public API (`filsys_read_ino`,
  `filsys_write_ino`, `filsys_stat_ino`, `filsys_truncate_ino`,
  `filsys_open_ino`/`filsys_close_ino`) — the exact shape a vnode-based driver
  wants, because the kernel already resolved the path to an inode.

## 1.2 Why a native driver when FUSE2 already works

| | FUSE2 (today) | native `sys/filsys/` (this plan) |
|---|---|---|
| kernel/user round-trip per op | yes | no |
| mount at boot / in `/etc/fstab` | no (userspace daemon) | yes |
| usable as root filesystem | no | yes (read-only at first) |
| edition autodetect at mount | `-v` / `-o` only | `filsys_detect()` available |
| crash/consistency semantics | FUSE's, not the fs's | owned by the driver |
| dependency | base libfuse present | none beyond the driver |

The native driver is not a rewrite of the format knowledge — that stays in
libfilsys. It is a **second transport** for the same engine, beside FUSE.

## 1.3 Non-goals

- **Not** a kernel port of `mkfs.filsys`, `fsck.filsys`, or `findfs.filsys`.
  Those stay userspace: they run *offline* on an unmounted image or block
  device, they need interactive prompts (`fsck -i`) and stdio, and there is no
  reason to carry that into the kernel. Only the *runtime* engine is ported.
- **Not** a fork of libfilsys's format code. GEFS copy/pasted Plan 9 code and
  rewrote it in place; this plan explicitly does the opposite.
- **Not** FFS or Minix (already out of scope per `ROADMAP.md`).
