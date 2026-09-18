# A native OpenBSD filesystem for filsys, reusing libfilsys as-is

Status: **research + design plan** (no code written yet)
Date: 2026-09-17
Audience: filsys maintainers; assumes the reader knows the OpenBSD VFS and the libfilsys engine.

## TL;DR

filsys already mounts on OpenBSD today through **FUSE2** (OpenBSD's in-base
`libfuse`, 2.6-era, `fuseops_openbsd.c`). libfilsys is the **backend**, one
format engine, behind **eight frontends** in two families: **callback** (FUSE,
and native `unixfs` drivers on OpenBSD/NetBSD/FreeBSD/Linux/Haiku) and
**message** (plan9, QNX).
This plan is the first callback-native driver: an in-kernel OpenBSD driver,
`sys/unixfs/`, that links the libfilsys format engine **unmodified** and only
adds the thin glue OpenBSD demands of a filesystem (`struct vfsops` +
`struct vops` + five registration edits).

The reference implementation is **GEFS-on-OpenBSD** (Ori Bernstein, announced
2026-09-15): a copy-on-write Plan 9 filesystem ported into the kernel in
~8,500 lines behind the exact same five-edit registration surface. GEFS
confirms the *integration surface*, the five registration edits are accurate,
not the recipe: it is a "rough, buggy, issue-filled preview" that is not in
master, and its `vfc_typenum` collides with `MOUNT_MSDOS` (§2.5). This plan
borrows the five edits and swaps GEFS's hand-ported copy/paste engine for
libfilsys compiled **as-is**.

The key enabler is already in libfilsys: all raw block I/O flows through a
two-function **`filsys_io_t` vtable** (`read`/`write` at a byte offset), whose
default is `pread`/`pwrite` on a host fd. A kernel driver supplies its own
`filsys_io_t` backed by `bread`/`VOP_STRATEGY`, and the on-disk codecs, V7,
V6, V1, PDP-7, 2.11BSD, System III/V, Coherent, Xenix, are untouched.

The honest boundary: **"as-is" covers the format engine** (codecs, allocators,
directory formats, byte-order vtable). It does *not* cover the userspace
convenience layer, the public `filsys.h` surface (path resolution, `struct
stat`/`statvfs` filling, FUSE callbacks), because that header includes
`<sys/stat.h>`, `<sys/statvfs.h>`, `<time.h>`, `<unistd.h>`, which do not exist
in the kernel. That layer is rebuilt kernel-side, thin, on top of the engine.

## Table of contents

Each section is a separate file so it can be read, reviewed, and revised on its
own. Read in order; later sections build on earlier ones.

0. **[Backend refactoring (first)](00-backend-refactoring.md)**, make the
   libfilsys core node-anchored, POSIX-free, offset-resumable, right-sized, and
   vendorable C99, and expose `bmap`; resolves risks 1, 2, 6 and supersedes
   §4.3/§4.4.
1. **[Context and motivation](01-context.md)**, what libfilsys already is, why a
   native driver when FUSE2 works, non-goals, and the other six ports
   (NetBSD/FreeBSD/Linux/Haiku/plan9/QNX) with their order.
2. **[Reference implementation: GEFS on OpenBSD](02-gefs-reference.md)**, the
   `sys/gefs/` layout, the five core-kernel edits, the two vtable contracts,
   device I/O, and the shortcuts we deliberately avoid.
3. **[The recipe, and a survey of the other OpenBSD filesystems](03-recipe-and-survey.md)**,
   the distilled 7-step recipe; a size/role table of FFS, ext2fs, msdosfs,
   tmpfs, cd9660, udf, ntfs, FUSE, GEFS.
4. **[What "using libfilsys as-is" means](04-as-is-boundary.md)**, the precise
   engine-vs-shim split, the `filsys_io_t` seam, and how the frontend maps its
   semantics to the backend (path vs. inode).
5. **[Proposed architecture: `sys/unixfs/`](05-architecture.md)**, file layout,
   per-mount state, the I/O transport, the allocation/logging/stat shims,
   locking, edition selection, and the five registration diffs.
6. **[Phased plan, risks, and open questions](06-phases-and-risks.md)**, four
   phases with acceptance criteria, then the hard decisions.
7. **[Test strategy and appendices](07-tests-and-appendix.md)**, how to verify
   without regressing userspace; the exact diff shapes, vtable field lists, the
   libc-dependency inventory, and references.
8. **[The other six ports](08-other-platforms.md)**, NetBSD, FreeBSD, Linux,
   Haiku (`unixfs`), plan9, QNX: how each is implemented on the §0 backend
   shape, and why this order.

## How this was produced

The GEFS port was pulled from `git://shithub.us/ori/openbsd` (branch `gefs`) and
its patch (`https://orib.dev/gefs.diff`, 9,094 lines) extracted to `sys/gefs/`
for reading. The upstream `openbsd/src` tree was cloned for the filesystem
survey in §3/§4. The libfilsys seam (§4/§5) was verified against this checkout's
`filsys_common.h` and `v7fs.c`.
