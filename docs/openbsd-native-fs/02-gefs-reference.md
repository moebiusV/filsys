# 2. Reference implementation: GEFS on OpenBSD

Repo: `git://shithub.us/ori/openbsd`, branch `gefs` (HEAD `0a08773f0…`).
Patch: `https://orib.dev/gefs.diff` (9,094 lines). Paper:
`https://orib.dev/gefs.pdf`.

GEFS ("Good Enough File System") is a crash-safe, snapshotting, copy-on-write
filesystem built on Bε trees, originally for 9front. The OpenBSD port was
announced 2026-09-15 on `openbsd-tech` as "a rough, buggy, issue-filled
preview" — not production-ready, data loss expected on error.

## 2.1 What it adds to the tree

A self-contained `sys/gefs/` directory of 12 `.c` files + 4 headers:

| file | lines | role |
|---|---|---|
| `vnops.c` | 1752 | `gefs_vops` + `gefs_devops` + `gefs_fifoops` |
| `tree.c` | 1612 | Bε tree |
| `blk.c` | 1082 | block cache + device I/O |
| `dat.h` | 800 | on-disk + in-core types |
| `snap.c` | 685 | snapshots |
| `sweep.c` | 663 | background reclaimer |
| `pack.c` | 591 | block serialization |
| `load.c` | 417 | `gefs_vfsops` (mount/unmount/root/statfs/…) |
| `dump.c` | 385 | on-disk dump (fsck-side) |
| `avl.c` | 352 | AVL tree |
| `fns.h` | 214 | prototypes |
| `hash.c` | 141 | block-pointer hashing |
| `util.c` | 84 | misc |
| `error.c` / `u.h` / `avl.h` | 114 | Plan 9 error/`u.h` shim |

## 2.2 The five core-kernel edits (the entire integration surface)

This is the crux. A filesystem's footprint in the *rest* of the kernel is five
tiny diffs:

1. **`sys/conf/files`** — list the sources, gated on a config token:
   ```
   file gefs/avl.c   gefs
   file gefs/blk.c   gefs
   ... (12 entries)
   ```
2. **`sys/conf/GENERIC`** — `option GEFS`.
3. **`sys/kern/vfs_init.c`** — one entry in `vfsconflist[]`:
   ```c
   { &gefs_vfsops, MOUNT_GEFS, 4, 0, MNT_LOCAL | MNT_SWAPPABLE,
       sizeof(struct gefs_args) },
   ```
4. **`sys/sys/mount.h`** — `struct gefs_args { char *fspec; struct export_args export_info; }`,
   a `union mount_info` member, `#define MOUNT_GEFS "gefs"`, and
   `extern const struct vfsops gefs_vfsops;`.
5. **`sys/sys/vnode.h`** — `VT_GEFS` in `enum vtagtype` + the `VTAG_NAMES` list.

Nothing else in the kernel is touched. Everything GEFS-specific lives under
`sys/gefs/`.

## 2.3 The two tables every filesystem implements

`struct vfsops` (filesystem-level; `load.c:403`):

```c
const struct vfsops gefs_vfsops = {
    .vfs_mount   = gefs_mount,     .vfs_start  = gefs_start,
    .vfs_unmount = gefs_unmount,   .vfs_root   = gefs_root,
    .vfs_quotactl= NULL,           .vfs_statfs = gefs_statfs,
    .vfs_sync    = gefs_sync,      .vfs_vget   = gefs_vget,
    .vfs_fhtovp  = NULL,           .vfs_vptofh = NULL,
    .vfs_init    = NULL,           .vfs_sysctl = NULL,
    .vfs_checkexp= NULL,
};
```

`struct vops` (per-file; `vnops.c:1640`) — the full `gefs_vops` table
implements `lookup create mknod open close access getattr setattr read write
ioctl kqfilter revoke fsync remove link rename mkdir rmdir symlink readdir
readlink abortop inactive reclaim lock unlock bmap strategy print islocked
pathconf advlock bwrite`. GEFS stubs `link`/`ioctl`/`kqfilter`/`advlock`/
`pathconf` with `eopnotsupp` (hardlinks are a known TODO), uses
`vop_generic_*` for `abortop`/`bmap`/`bwrite`, and `nullop` for
`lock`/`unlock`/`islocked`. It also carries two *derived* tables —
`gefs_devops` (mirrors `spec_vops` for device nodes) and `gefs_fifoops`
(mirrors `fifo_vops`) — the standard trick every OpenBSD fs with device/FIFO
support uses.

## 2.4 Device I/O

GEFS opens the block device with `VOP_OPEN(devvp, FREAD|FWRITE, …)`, reads the
disklabel via `VOP_IOCTL(devvp, DIOCGDINFO, …)` to learn the size, and does all
block traffic through the standard **buf cache**: `bread(fs->dev, …)`,
`getblk(fs->dev, …)`, `geteblk(...)`, then `VOP_STRATEGY(fs->dev, bp)` +
`biowait(bp)` on write. This is identical to `ffs`/`msdosfs`/`ext2fs`. There is
no bespoke I/O path.

## 2.5 What GEFS took shortcuts on (and this plan avoids)

GEFS's port is deliberately minimal, and several choices are *not* things to
copy for a durable filsys driver:

- **Global single-instance state** — `Gefs *fs` is a file-scope global;
  `gefs_mount` returns `ENODEV` if `fs != NULL` ("only one gefs at a time").
- **Global single-writer lock** — `fs->mutlk` (an `rwlock`) serializes *every*
  mutation, with per-dentry rwlocks layered on top. Simple, but SMP-hostile.
- **Background kthreads** — `kthread_create(gefs_sweep, …)` plus an "admin
  channel" (`fs->admchan`) for sync/halt. Needed by CoW sweeping; filsys's
  free-list/bitmap formats need no such machinery.
- **Many `printf`s** left in mount/unmount/statfs (bringup noise).
- **Error handling commented out**, `access`/permissions stubbed, hardlinks and
  kqueue `eopnotsupp`.

filsys's formats are far simpler than a CoW Bε tree: a static superblock, a
free list or bitmap, and fixed inode tables. The driver should be a *smaller*
job than GEFS, not a larger one — see §5 in [05-architecture.md].
