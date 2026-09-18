# 3. The OpenBSD filesystem recipe, and a survey of the others

## 3.1 The recipe (distilled)

A new local filesystem is, in full:

1. **`sys/<name>/`**, self-contained `.c`/`.h`, including `struct vfsops` and
   one or more `struct vops`.
2. **`sys/conf/files`**, `file <name>/<f>.c  <name>` per source, gated on the
   config token.
3. **`sys/conf/GENERIC`** (and `RAMDISK` etc. as wanted), `option <NAME>`.
4. **`sys/kern/vfs_init.c`**, a `vfsconflist[]` entry.
5. **`sys/sys/mount.h`**, `struct <name>_args`, a `union mount_info` slot,
   `MOUNT_<NAME>` string, and the `_vfsops` extern.
6. **`sys/sys/vnode.h`**, a `VT_<NAME>` vtag.
7. **`sbin/mount_<name>/`**, a userspace `mount_<name>(8)` that parses options
   into `<name>_args` and calls `mount(2)`.

Steps 1-6 are the driver; step 7 is the only *required* userspace piece (and it
is optional if `mount(8)` is hand-driven).

## 3.2 Survey of the other OpenBSD filesystems

Sizes (`.c`+`.h` under `src/sys/…`, current `openbsd/src`):

| filesystem | path | lines | rw | notes |
|---|---|---|---|---|
| FFS / FFS2 | `ufs/ffs/` | 6,117 | rw | flagship; quotas, NFS fh (`fhtovp`/`vptofh`), soft updates |
| ext2fs | `ufs/ext2fs/` | 7,128 | rw | shares `ufs/` inode/mount plumbing |
| msdosfs | `msdosfs/` | 7,452 | rw | FAT; name mangling + codepage conversion |
| tmpfs | `tmpfs/` | 5,257 | rw | in-memory; **pool allocator** (`pool(9)`), no disk I/O |
| cd9660 | `isofs/cd9660/` | 4,674 | ro | Rock Ridge; the canonical read-only driver |
| udf | `isofs/udf/` | 3,581 | ro | read-only |
| ntfs | `ntfs/` | 4,377 | ro | read-only |
| **FUSE** | `miscfs/fuse/` | 3,565 | (shim) | the kernel half of FUSE; filsys's *current* path |
| **GEFS** | `gefs/` | ~8,500 | rw | CoW + snapshots (reference for this plan) |

Observations that bear on filsys:

- **A read-only driver is the smallest honest first step.** cd9660 and ntfs
  implement only a read subset of `vops` (`read`, `getattr`, `readdir`,
  `lookup`, `readlink`, `bmap`, `inactive`, `reclaim`); everything mutating is
  `vop_generic_badop` or absent. filsys's V7/V6/etc. images are exactly the
  kind of thing one mounts read-only to copy files off (§6 in
  [06-phases-and-risks.md] phases the driver this way).
- **The vfsops surface is uniform.** Every one of ffs/ext2fs/msdosfs/tmpfs/
  ntfs/cd9660/udf/gefs fills the *same* `struct vfsops`; the variation is only
  in which `vops` members are stubbed. This confirms the recipe in §3.1 is
  complete.
- **tmpfs is the template for "no device".** Its `vfs_mount` takes no `fspec`
  and does no `VOP_OPEN`, irrelevant for filsys (which is device-backed) but a
  reminder that `mount` argument shape is per-fs and free-form.

## 3.3 Where the comparison matters

The single takeaway for this plan: **GEFS is not unique in structure; it is a
standard-issue OpenBSD filesystem that happens to be small.** Every fact the
plan leans on (five registration edits, two vtable contracts, buf-cache device
I/O, a `mount_<name>` tool) is the same across ffs, msdosfs, cd9660, tmpfs, and
ntfs. So the plan's confidence does not rest on one idiosyncratic port; it rests
on the platform's own uniform contract.
