# 1. Context and motivation

## 1.1 What libfilsys already is

libfilsys is a version-agnostic access layer for Research Unix filesystem
images: PDP-7 through Tenth Edition, plus 32V, Coherent, early Xenix,
2.9/2.11BSD, System III and System V. One engine handles 16-, 18-, 32- and
64-bit word/block addressing in big-, little- and middle-endian order. It ships
four userspace tools, `mount.unixfs` (FUSE), `mkfs.unixfs`, `fsck.unixfs`,
`findfs.unixfs`, plus a FUSE-free core (`test_matrix` + the mkfs/fsck
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
  self-contained, no globals in the format engine.

- **The mutation ops decompose to inode-anchored primitives.** The public
  path-based API (`filsys_create`, `filsys_unlink`, `filsys_rename`, …) is a
  thin layer over `dir_lookup` / `dir_add` / `dir_remove` (the `filsys_dir_ops`
  vtable), `ialloc` / `ifree`, `bmap`, and `filsys_read_inode` /
  `filsys_write_inode`. The kernel driver needs those primitives, not path
  strings.

- **Inode-based read/write already exist** as public API (`filsys_read_ino`,
  `filsys_write_ino`, `filsys_stat_ino`, `filsys_truncate_ino`), the exact
  shape a vnode-based driver wants, because the kernel already resolved the
  path to an inode. (`filsys_open_ino`/`filsys_close_ino` move to the FUSE
  frontend in §0; the kernel vnode itself is the pin.)

## 1.2 Why a native driver when FUSE2 already works

libfilsys is the **backend**, one format engine that does the correct thing per
edition, behind several **frontends** in two families: **callback** (FUSE,
native drivers) and **message** (plan9, QNX), each mapping its own semantics
onto it. This plan is the first callback-native driver (OpenBSD); the other
ports are §1.4. Where a frontend's semantics differ from the engine's,
the frontend maps; the engine does not encode any one frontend's rules.

| | FUSE2 (today) | native `sys/unixfs/` (this plan) |
|---|---|---|
| kernel/user round-trip per op | yes | no |
| mount at boot / in `/etc/fstab` | no (userspace daemon) | yes |
| usable as root filesystem | no | possible, not planned |
| edition autodetect at mount | `-v` / `-o` only | `filsys_detect()` available |
| crash/consistency semantics | FUSE's, not the fs's | owned by the driver |
| dependency | base libfuse present | none beyond the driver |

The native driver is not a rewrite of the format knowledge; that stays in
libfilsys. It is a **second frontend** for the same backend, beside FUSE.

## 1.3 Non-goals

- **Not** a kernel port of `mkfs.unixfs`, `fsck.unixfs`, or `findfs.unixfs`.
  They stay userspace; they run *offline* on an unmounted image or block
  device and need interactive prompts (`fsck -i`) and stdio, but they are not
  dropped: the port ships them as userspace companions to the driver (they
  already size a raw block device via `filsys_dev_size`, §4.1). Only the
  *runtime* engine is ported *into the kernel*.
- **Not** a fork of libfilsys's format code. GEFS copy/pasted Plan 9 code and
  rewrote it in place; this plan explicitly does the opposite.
- **Not** FFS or Minix (already out of scope per `ROADMAP.md`).

## 1.4 The other ports, and their order

The backend serves five native kernel drivers (the OpenBSD driver, this plan's
subject §5-§6, plus NetBSD, FreeBSD, Linux, Haiku) and two userspace servers
(plan9, QNX), plus one FUSE frontend (`filsys`), which also runs on Haiku's
FUSE 2.9.9. The seven are not seven of the same thing, and the order is
deliberate: OpenBSD first, NetBSD second, FreeBSD third, Haiku fourth, the
message family (plan9 then QNX), Linux last. The per-platform "how to
implement" is §8.

- **NetBSD**, the close cousin; `struct vnodeopv_entry_desc` arrays rather than
  a flat `vops`, and **rump kernels** as the cheap test loop.
- **FreeBSD**, the other close cousin; a flat `struct vop_vector` (near
  OpenBSD's `struct vops`), so the glue is closer to OpenBSD than NetBSD's.
- **Haiku**, a kernel filesystem add-on (`file_system_module_info` /
  `fs_vnode_ops`), callback-shaped; also a FUSE target (2.9.9) via the `filsys`
  frontend.
- **plan9**, a userspace 9P server (message family); the fid is the handle;
  no kernel shims.
- **QNX**, a resource manager (message family); the OCB is plan9's fid; done
  after plan9 because the mapping is then largely written.
- **Linux**, last, not because the rationale is weak but because the bar is
  highest and the argument is the kernel's own history: the in-tree `sysv`
  driver for these formats was removed as unused-and-unsafe, and the "boring"
  standard (§8.3) is how the replacement answers that. Out-of-tree first, then
  `fs/unixfs/`, default off.
- **macOS**, FUSE-only for now (macFUSE/FUSE-T, both FSKit-backed); the native
  path would be FSKit, off the critical path (§8.7).

`unixfs` is the filesystem name on every frontend, native and FUSE alike, and
`filsys` is the project, engine and library name. IPFS's "UnixFS" is an internal
data format, not a mount name, so there is nothing to avoid. Haiku runs both,
a native `unixfs` add-on and a FUSE `unixfs` mount.

Each maps its own semantics onto the same backend shape §0 establishes; nothing
here changes the OpenBSD driver, only the backend it shares with these six.
