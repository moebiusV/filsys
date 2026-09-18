# 6. Phased plan

## Phase 0 — feasibility spike (this document)

Deliverable: this plan, validated against the real GEFS port and the real
libfilsys seam (`filsys_io_t`). No code.

**Acceptance:** the `filsys_io_t` seam is confirmed to cover *all* block I/O;
the engine's non-I/O libc surface is enumerated (§4.2, §A.3).

## Phase 1 — read-only mount, one edition (V7)

- Implement `filsys_io_kern` (read path first), the alloc/log/time shims, and a
  read-only `unixfs_vfsops` + `unixfs_vops` with the read subset: `lookup,
  open, close, access(→0 or generic), getattr, read, readdir, readlink, bmap,
  inactive, reclaim, lock/unlock/islocked` (real, `rrwlock`-based — §5.5), plus
  the generic stubs `abortop, pathconf, strategy, print, revoke, bwrite`. A
  NULL `vop_lock` makes `vn_lock` return `EOPNOTSUPP` and takes `namei` with
  it, and the first several boots end in `ddb` on whichever slot is still NULL
  — fill them here, not one panic at a time (§0).
- Wire the five registration edits; add `sbin/mount_unixfs/`.
- Build a `GENERIC`-with-UNIXFS kernel and boot it (or a `vnd(4)`-backed test
  in a VM).

**Acceptance:** a V7 `rp06-0.disk` mounted read-only; `ls`, `cat`, `cp` out,
`stat`/`du` correct; `make check` unchanged (the userspace build is untouched).

## Phase 2 — read-write, one edition (V7)

- Add the mutating `vops`: `create, mkdir, mknod, remove, rmdir, rename,
  link, symlink, setattr, write, fsync, truncate`.
- Per-mount exclusive `rwlock` for mutations; the vnode is the open-file pin
  (no `filsys_open_ino`/`close_ino` — §0 moved them to the FUSE frontend);
  superblock `mark_dirty`/`mark_clean` on mount/unmount.
- Write `VOP_RENAME` last: it is the ugliest op in the vnode interface (up to
  four vnodes with a locking and reference-dropping protocol the filesystem must
  release on every path, plus `..` fix-up), and filsys's rename is currently
  path-based and re-resolves both parents — the one op where the node-anchored
  core (§0) pays for itself.

**Acceptance:** the existing FUSE `test.sh` matrix (read/write/mkdir/rename/
truncate/persistence) passes against the *native* mount on a V7 image; the
image is then remountable and `fsck.filsys` (userspace) reports it clean.

## Phase 3 — the rest of the editions

- Enable the remaining backends (`v1`, `v6`, `pdp7`, `32V`, `bsd29`,
  `bsd211`, `v8`, `v9`, `v10`, `coherent`, `xenix`, `sysiii`, `svr2`, `svr4`).
- Exercise the byte-order matrix (little/big/middle) and word-size paths
  through the native driver.

**Acceptance:** the `test_matrix` oracle (the format round-trips) passes for
every edition through the *kernel* I/O transport (run the userspace
`test_matrix` against a `filsys_io_kern`-shaped transport in a harness, or
mount each edition on a VM and diff against the userspace result).

## Phase 4 — hardening and packaging

- SMP smoke tests (concurrent readers; concurrent writers under the big lock).
- NFS re-export (fill `vfs_fhtovp`/`vfs_vptofh` — the inode number is a natural
  file handle) if desired; otherwise leave `NULL` like GEFS.
- Manpage `mount_unixfs(8)`, `GENERIC`/`RAMDISK` entries, and a packaging
  follow-up in `packaging/openbsd/` that ships the native `mount_unixfs(8)` plus
  the userspace `fsck.filsys`/`mkfs.filsys`/`findfs.filsys` — a mount driver
  alone is not enough; the companion tools have to be in the port too.

**Acceptance:** `./configure && make && make install` still yields the FUSE
toolchain untouched; the kernel driver is a separate, documented artifact.

---

# 7. Risks and open questions

1. **The public-header boundary.** `filsys.h` cannot be `#include`d in-kernel
   (`<sys/stat.h>` etc.). §0 resolves this: a POSIX-free `filsys.h` (plain
   integer `filsys_inode_t`/`filsys_statfs_t`, each frontend filling its own
   type) removes the need for both a separate `unixfs_kern.h` and `#ifdef
   UNIXFS_KERNEL` guards.
2. **Allocation seam.** The engine has eighteen allocation sites (6 `malloc`,
   10 `calloc`, 2 `realloc`) and 45 `free`s — an order of magnitude below the
   old §A.3 estimate. `fs` is already in scope at fifteen of them. §0 resolves
   this with a build-selected `filsys_alloc`/`filsys_free`, not a `#define`:
   the two `realloc`s go away (one moves to the FUSE frontend, one becomes
   alloc-copy-free) and the remaining sites gain a parameter they already have
   in scope.
3. **Detection in-kernel vs in-`mount_unixfs`.** Prefer userspace detection to
   keep the kernel driver small, not to avoid fd usage — the probe reads through
   the build-selected transport and comes along free; only `filsys_detect.c`'s
   thin wrapper is fd-bound (§5.7).
4. **Concurrency correctness — CLOSED.** The engine is not reentrant; the big
   per-mount lock is correct but serial. The read path under shared lock was
   audited and does not mutate engine state: reads touch only the decoded inode
   cache (write-invalidate on setattr/truncate) and never the free-list cache,
   so shared-lock read-only is sound. Writers serialize on the exclusive lock.
5. **Block-size vs `DEV_BSIZE`.** libfilsys block sizes ≠ 512; the transport's
   sub-block assembly is the one place bugs will concentrate. Fuzz it with the
   fault-injection transport (`filsys_set_io`) before the kernel build.
6. **`filsys_edition_t` allocator union.** The freelist cache
   (`V8_NICFREE_LARGE = 946` entries) is a large stack/struct member; in-kernel
   it should be heap-allocated (a `pool`), not embedded, to keep `M_FILSYS`
   allocations bounded and pageable.
7. **SemVer / distribution.** The engine is compiled from libfilsys source into
   the kernel; decide pinning up front (§5.1) — a vendored snapshot under
   `sys/unixfs/` with a pin and a sync script, or a patch against `-current`.
   `ROADMAP.md`'s strict SemVer means the engine API must stay source-stable —
   the additive inode-anchored exports (§4.3) and the `ops->probe` signature
   change (the build-selected transport drops its `filsys_io_t *` argument,
   §5.7) must land in the same MINOR, not a patch.
8. **Kernel allocation failure mode.** The directory readers allocate the whole
   directory in one `malloc`, bounded only by the superblock check
   `ip->size ≤ (fsize − data_start) × bsize ≤ imgsize` (`v7fs.c:180`). In-kernel
   `km_alloc(kd_waitok)` sleeps where `malloc` returns NULL, so a corrupt
   `di_size` that passes the bound hangs instead of failing with `-ENOMEM`. The
   shim wants either an absolute cap on top of the relative one, or `dir_read`
   extended to the chunked scan that `dir_lookup`/`dir_add`/`dir_remove` already
   use — which removes the allocation rather than bounding it.
9. **The device case loosens the size bound.** A V7 image file is a few MB and
   `fstat` says so; a partition is whatever the disklabel says, so `fsize` can
   legitimately be declared much larger and a corrupt `di_size` has more room.
   This is robustness, not security — mount is root-only (no `kern.usermount`).
   But the engine's corrupt-media guards must be *kept*, not assumed away by
   "device-backed means well-formed": the normal input is `vnd(4)` over a file
   of unknown provenance (§6's "`rp06-0.disk` mounted read-only" is `vnconfig`,
   not physical media).
