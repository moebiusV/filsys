# 6. Phased plan

## Phase 0 — feasibility spike (this document)

Deliverable: this plan, validated against the real GEFS port and the real
libfilsys seam (`filsys_io_t`). No code.

**Acceptance:** the `filsys_io_t` seam is confirmed to cover *all* block I/O;
the engine's non-I/O libc surface is enumerated (§4.2, §A.3).

## Phase 1 — read-only mount, one edition (V7)

- Implement `filsys_io_kern` (read path first), the alloc/log/time shims, and a
  read-only `filsys_vfsops` + `filsys_vnops` with the read subset:
  `lookup, open, close, access(→0 or generic), getattr, read, readdir,
  readlink, bmap, inactive, reclaim, lock/unlock(nullop), islocked`.
- Wire the five registration edits; add `sbin/mount_filsys/`.
- Build a `GENERIC`-with-FILSYS kernel and boot it (or a `vnd(4)`-backed test
  in a VM).

**Acceptance:** a V7 `rp06-0.disk` mounted read-only; `ls`, `cat`, `cp` out,
`stat`/`du` correct; `make check` unchanged (the userspace build is untouched).

## Phase 2 — read-write, one edition (V7)

- Add the mutating `vops`: `create, mkdir, mknod, remove, rmdir, rename,
  link, symlink, setattr, write, fsync, truncate`.
- Per-mount exclusive `rwlock` for mutations; wire `filsys_open_ino`/`close_ino`
  for hard-remove; superblock `mark_dirty`/`mark_clean` on mount/unmount.

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
- Manpage `mount_filsys(8)`, `GENERIC`/`RAMDISK` entries, and a packaging
  follow-up in `packaging/openbsd/` (the port that today ships the FUSE2 build).

**Acceptance:** `./configure && make && make install` still yields the FUSE
toolchain untouched; the kernel driver is a separate, documented artifact.

---

# 7. Risks and open questions

1. **The public-header boundary.** `filsys.h` cannot be `#include`d in-kernel
   (`<sys/stat.h>` etc.). Decide: a separate `filsys_kern.h` that includes only
   `filsys_common.h` + backend headers, or `#ifdef FILSYS_KERNEL` guards in
   `filsys.h`. The former keeps the public header clean; the latter risks
   leaking kernel-isms into userspace. **Recommend:** a dedicated
   `filsys_kern.h`, no edits to `filsys.h`.
2. **`malloc` shimming.** `#define malloc` is gross but touches 0 call sites.
   The alternative (threading an allocator through the codecs) is a fork.
   Decide early and document the macro shim as a deliberate seam (§5.4).
3. **Detection in-kernel vs in-`mount_filsys`.** Prefer userspace detection to
   keep `filsys_detect.c` (and its `open`/fd usage) out of the kernel (§5.7).
4. **Concurrency correctness.** The engine is not reentrant; the big per-mount
   lock is correct but serial. Confirm the read path under shared lock never
   mutates engine state (the free-list cache is the thing to audit — reads must
   not touch it).
5. **Block-size vs `DEV_BSIZE`.** libfilsys block sizes ≠ 512; the transport's
   sub-block assembly is the one place bugs will concentrate. Fuzz it with the
   fault-injection transport (`filsys_set_io`) before the kernel build.
6. **`filsys_edition_t` allocator union.** The freelist cache
   (`V8_NICFREE_LARGE = 946` entries) is a large stack/struct member; in-kernel
   it should be heap-allocated (a `pool`), not embedded, to keep `M_FILSYS`
   allocations bounded and pageable.
7. **SemVer / distribution.** The engine is compiled from libfilsys source into
   the kernel; decide pinning (vendor a pinned tarball revision vs. build the
   kernel against the checkout). `ROADMAP.md`'s strict SemVer means the engine
   API must stay source-stable — the additive inode-anchored exports (§4.3) must
   land in a MINOR, not a patch.
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
