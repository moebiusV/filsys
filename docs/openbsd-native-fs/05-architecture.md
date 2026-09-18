# 5. Proposed architecture: `sys/filsys/`

Mirror `sys/gefs/` but with a per-mount instance (no global) and no background
threads.

## 5.1 File layout

```
sys/filsys/
  filsys_vfsops.c     # struct vfsops: mount/unmount/root/statfs/sync/vget  (~mirrors gefs load.c)
  filsys_vnops.c      # struct vops: lookup/create/.../reclaim            (~mirrors gefs vnops.c)
  filsys_vnode.h      # struct filsys_node { filsys_inode_t ino; ... }  (v_data payload)
  filsys_kern.h       # the shim: alloc/log/time + filsys_io_kern declaration
  filsys_io_kern.c    # filsys_io_t over bread/VOP_STRATEGY
  <engine, compiled as-is, via sys/conf/files pointing back at the libfilsys sources>
```

The engine sources are *not* copied into the kernel tree. They are referenced
from the libfilsys tree (or vendored at a pinned revision) by `sys/conf/files`
entries; the only new files are the four above. This is what "as-is" buys: no
second copy of `v7fs.c` to drift.

## 5.2 Per-mount state

Replace GEFS's global `Gefs *fs` with a `struct filsys_mount` embedded in
`mp->mnt_data`, holding one `filsys_edition_t` (already self-contained), the
device vnode, the resolved edition/geometry, and a single per-mount `rwlock`.
Each `filsys_node` (in `vp->v_data`) holds the inode number + a cached decoded
`filsys_inode_t`.

## 5.3 I/O transport (`filsys_io_kern.c`)

`filsys_io_t.read(fs, buf, n, off)` → the kernel driver:

- clamps to `[fs->base, fs->base+imgsize)` (the `imgsize` the mount learned
  from `DIOCGDINFO`, or an `-o size=` override),
- for each `DEV_BSIZE`-aligned span, `bread(devvp, off/DEV_BSIZE, DEV_BSIZE, …)`
  and copy the partial head/tail,
- `write` mirrors with `getblk` + `VOP_STRATEGY` + `biowait`, honoring the
  `MNT_RDONLY` / `filsys_edition_t.readonly` flag.

Because libfilsys's block sizes (128/256/512/1024/4096/8192) need not equal
`DEV_BSIZE` (512), the transport does the sub-block assembly — the codec's
"logical block" abstraction is preserved unchanged.

## 5.4 Allocation shim (`filsys_kern.h`)

```
#define filsys_malloc(n)      km_alloc((n), &kv_filsys, &kp_dirty, &kd_waitok)
#define filsys_free(p, n)     km_free((p), (n), &kv_filsys, &kp_dirty)
```
or `pool(9)` for the fixed-size hot paths. The engine's `malloc(n)` /
`free(p)` are the *only* thing mapped; call sites are untouched. (This is the
one place a `#define` is justified — there is no thread through the codecs to
pass an allocator, and touching 300 call sites would be a fork.)

## 5.5 Locking and concurrency

The kernel is preemptive/SMP; the engine is single-threaded and reentrancy-free
today (the FUSE adapter serializes via FUSE's own single-threaded loop). The
driver must therefore serialize engine entry:

- **Phase 1 (read-only):** a single per-mount `rwlock` held in shared mode
  across read/lookup/getattr/readdir; no write path exists, so no writers.
- **Phase 2 (read-write):** the same per-mount `rwlock` held exclusively across
  every mutation, *à la* GEFS's `fs->mutlk`, plus per-node `rwlock`s only where
  profiling shows. Do not copy GEFS's *global* lock; make it per-mount so
  two filsys mounts are independent.

This is a deliberate, documented simplification: filsys's target images are
small, historical, and usually mounted for copy-in/copy-out, not as a hot
production root. A big per-mount lock is the correct first cut; it can be
refined later without changing the on-disk behavior.

## 5.6 Stat / time / statfs mapping

- `VOP_GETATTR`: `vattr_null` + fill from `filsys_read_inode` into
  `va_mode/va_nlink/va_uid/va_gid/va_size/va_atime/va_mtime/va_ctime/va_blocks`.
  (A tiny helper `filsys_fill_vattr(fs, ip, struct vattr *)` lives in the
  kernel glue; it mirrors the userspace `filsys_fill_stat`.)
- `VOP_SETATTR`: map `va_size/va_mode/va_uid/va_gid/va_atime/va_mtime` onto
  `filsys_truncate_ino`/`filsys_chmod`/`filsys_chown`/`filsys_utimens` — but
  note the public chmod/chown/utimens are path-based; the inode-anchored
  exports from §4.3 cover this too.
- `VOP_STATFS` (`vfs_statfs`): fill `f_bsize/f_iosize/f_blocks/f_bfree/
  f_files/f_ffree` from the per-edition `statfs` op (`filsys_statfs`), then
  `copy_statfs_info(sbp, mp)` as GEFS/ffs do.
- Times: the engine already stores atime/mtime/ctime as epoch seconds
  (`filsys_inode_t`); convert with `nanotime(9)` on write.

## 5.7 Edition selection and geometry

`struct filsys_args` (in `mount.h`) carries: `fspec`, `edition` (a `FILSYS_*`
selector), `readonly`, `offset`, `arch`, and the V8-family `geom`
(blocksize/freemap/byteorder). The `mount_filsys(8)` tool parses
`-o edition=v7`, `-o offset=0`, `-o arch=vax`, `-o readonly`, etc., into this
struct. Two mount modes:

- **explicit** — `-o edition=v7` (default for determinism, matches the FUSE
  tool's `-v`);
- **autodetect** — `-o autodetect` calls `filsys_detect()` at mount; but
  `filsys_detect.c` is currently an offline, userspace file (it opens an fd),
  so the probe must either be pulled into the runtime subset or done by
  `mount_filsys` in userspace before `mount(2)`. Prefer the latter: keep
  detection in userspace, pass the resolved edition in `filssys_args`. (This
  keeps the kernel driver small and the probe logic out of the kernel.)

## 5.8 The vnode payload and inode life cycle

- `VOP_LOOKUP`: `filsys_lookup`-equivalent on the parent inode (via the
  inode-anchored `dir_lookup`), then `getnewvnode(VT_FILSYS, mp,
  &filsys_vops, vpp)` and cache the decoded inode in `vp->v_data`.
- `VOP_RECLAIM`/`VOP_INACTIVE`: free the node's cached state; mirror GEFS's
  `gefs_clunkdent`/`gefs_reclaim`.
- Hard-remove semantics (unlink of an open file) are already modeled by
  `filsys_open_ino`/`filsys_close_ino`; the driver's `VOP_REMOVE` bumps the
  handle and `VOP_INACTIVE`/`VOP_RECLAIM` drops it — the same deferred-free
  the FUSE adapter does today.
- Device nodes: a `filsys_devops` mirroring `spec_vops` (copy GEFS's
  `gefs_devops` pattern) for V6/V7 char/block specials; FIFOs via
  `filsys_fifoops` only if `option FIFO` is on (Coherent pipes).

## 5.9 The registration edits (the five diffs, filsys edition)

1. `sys/conf/files` — `file filsys/filsys_vfsops.c filsys`, `file
   filsys/filsys_vnops.c filsys`, `file filsys/filsys_io_kern.c filsys`, plus
   the libfilsys engine sources each tagged `filsys`.
2. `sys/conf/GENERIC` — `option FILSYS`.
3. `sys/kern/vfs_init.c` — `{ &filsys_vfsops, MOUNT_FILSYS, 0, 0, MNT_LOCAL,
   sizeof(struct filsys_args) }`.
4. `sys/sys/mount.h` — `struct filsys_args`, `union mount_info` slot,
   `#define MOUNT_FILSYS "filsys"`, `extern const struct vfsops filsys_vfsops;`.
5. `sys/sys/vnode.h` — `VT_FILSYS` + `VTAG_NAMES`.

Plus the one userspace tool `sbin/mount_filsys/` (§3.1 step 7).
