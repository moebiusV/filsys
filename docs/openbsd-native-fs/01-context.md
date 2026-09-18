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
  `filsys_write_ino`, `filsys_stat_ino`, `filsys_truncate_ino`) — the exact
  shape a vnode-based driver wants, because the kernel already resolved the
  path to an inode. (`filsys_open_ino`/`filsys_close_ino` move to the FUSE
  frontend in §0; the kernel vnode itself is the pin.)

## 1.2 Why a native driver when FUSE2 already works

libfilsys is the **backend** — one format engine that does the correct thing per
edition — behind several **frontends** in two families: **callback** (FUSE,
native drivers) and **message** (9P, QNX), each mapping its own semantics onto
it. This plan is the first callback-native driver (OpenBSD); the other three
native ports are §1.4. Where a frontend's semantics differ from the engine's,
the frontend maps; the engine does not encode any one frontend's rules.

| | FUSE2 (today) | native `sys/filsys/` (this plan) |
|---|---|---|
| kernel/user round-trip per op | yes | no |
| mount at boot / in `/etc/fstab` | no (userspace daemon) | yes |
| usable as root filesystem | no | yes (read-only at first) |
| edition autodetect at mount | `-v` / `-o` only | `filsys_detect()` available |
| crash/consistency semantics | FUSE's, not the fs's | owned by the driver |
| dependency | base libfuse present | none beyond the driver |

The native driver is not a rewrite of the format knowledge — that stays in
libfilsys. It is a **second frontend** for the same backend, beside FUSE.

## 1.3 Non-goals

- **Not** a kernel port of `mkfs.filsys`, `fsck.filsys`, or `findfs.filsys`.
  They stay userspace — they run *offline* on an unmounted image or block
  device and need interactive prompts (`fsck -i`) and stdio — but they are not
  dropped: the port ships them as userspace companions to the driver (they
  already size a raw block device via `filsys_dev_size`, §4.1). Only the
  *runtime* engine is ported *into the kernel*.
- **Not** a fork of libfilsys's format code. GEFS copy/pasted Plan 9 code and
  rewrote it in place; this plan explicitly does the opposite.
- **Not** FFS or Minix (already out of scope per `ROADMAP.md`).

## 1.4 The other three ports, and their order

The backend serves more than OpenBSD; the native-driver story is four ports, and
the four are not four of the same thing. The order below is deliberate —
OpenBSD first, the others only after the backend shape (§0) is proven.

- **NetBSD** — the close cousin, and second. Two differences matter: NetBSD
  registers `struct vnodeopv_entry_desc` arrays rather than OpenBSD's flat
  `struct vops`, so the glue is not copy-paste; and it offers **rump kernels**,
  which run the real NetBSD VFS with the filesystem linked in as a userspace
  process under a normal debugger. Rump is the best test loop of the four by a
  wide margin — better than the QEMU story for OpenBSD — so NetBSD goes second
  because the BSD VFS knowledge is still fresh and rump finds engine- and
  glue-level bugs that cost a VM reboot each on OpenBSD.
- **QNX** — not a kernel driver at all. QNX has no VFS: a filesystem is a
  resource manager, a userspace process that registers a pathname prefix with
  `procmgr` and answers `_IO_READ`/`_IO_WRITE`/`_IO_STAT`/`_IO_OPENFD` messages,
  usually via the `iofunc_*` helpers. It is message-shaped, runs in userspace,
  uses ordinary `malloc`, and needs none of the kernel shims. Its OCB is 9P's
  fid, so doing QNX after 9P means the message-family mapping is largely already
  written.
- **Linux** — last, and the one with the weakest rationale. Linux's FUSE is
  mature, fast, mounts from `/etc/fstab` via `mount.fuse`, and can serve as root
  from an initramfs. For read-mostly historical images a Linux native driver
  buys very little over the FUSE frontend already present, and costs a permanent
  tracking burden against an API that changes every release. This is not an
  argument against doing it — "V7 filesystems mount natively on Linux" is worth
  wanting, and the §0 work is what makes it small — it is an argument for not
  letting Linux set the schedule: OpenBSD has the real gap (base libfuse is
  2.6-era and slow), NetBSD has the cheap test loop, QNX rides on 9P, and Linux
  can wait until the other three have proven the backend shape.
