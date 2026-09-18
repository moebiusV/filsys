# 8. The other six ports: NetBSD, FreeBSD, Linux, Haiku, plan9, QNX

The backend serves five native kernel drivers (the OpenBSD driver, this plan's
subject §5-§6, plus NetBSD, FreeBSD, Linux, Haiku) and two userspace servers
(plan9, QNX). They are deliberately ordered, OpenBSD first, NetBSD second,
FreeBSD third, Haiku fourth, the message family (plan9 then QNX), Linux last,
because the seven are not seven of the same thing. Each section below is "how to
implement on this platform," written against the backend shape §0 establishes,
so the engine drops into each without re-doing the mapping.

## 8.1 NetBSD

The close cousin. Same VFS family, two differences that matter:

- **Registration is a table of descriptors, not a flat struct.** NetBSD
  registers `struct vnodeopv_entry_desc` arrays (a name per op) rather than
  OpenBSD's flat `struct vops`, so the glue is not copy-paste; it is a
  mechanical translation, and the op bodies themselves carry over.
- **rump kernels are the test loop.** Rump runs the real NetBSD VFS with the
  filesystem linked in as a userspace process under a normal debugger. That
  finds engine- and glue-level bugs that cost a VM reboot each on OpenBSD.

NetBSD goes second precisely because the BSD VFS knowledge is still fresh and
rump makes it the cheapest of the native set to iterate on. The transport is the
same `filsys_io_kern`-shaped `bread`/`VOP_STRATEGY` path, and the per-vnode lock
protocol is the same BSD-family `rrwlock` work (§5.5).

## 8.2 FreeBSD

FreeBSD is the other close cousin, and the nearest to OpenBSD structurally: a
flat `struct vop_vector` (nearly `struct vops` under another name) and
registration via `VFS_SET`. It also ships a mature in-tree `fusefs`, so the FUSE
frontend already works there and the native driver is the optional hardening
step, not the gap-filler. Its specific VFS details (mount args, `vfs_modevent`,
the vnode-lifecycle protocol) get their own pass when FreeBSD is scheduled.

## 8.3 Linux

Call the in-tree filesystem **`unixfs`**. `filsys` is the project and the on-disk
homage; Linux already burned the `sysv` name, and `unixfs` is what an admin
expects in `fstab` and `/proc/filesystems`. The name is free in-tree; the only other uses of "unixfs" are IPFS's internal
UnixFS data format (a serialization format, not a mount name, IPFS mounts via
`ipfs mount`, not `mount -t unixfs`) and NetBSD's dead 1990s `arm32` RISC
OS-ADFS mounter, so there is no functional collision. `unixfs` is the filesystem
name on every frontend, native and FUSE alike, while `filsys` stays the
project, engine and library name. Keep the engine symbol prefix `filsys_` so the
same objects compile into OpenBSD, NetBSD, FreeBSD, and this module. One
naming collision to avoid: `filsys` already uses `unix` as its autodetect
pseudo-edition (`FILSYS_UNIX = 40`, `-v unix`). `mount -t unixfs -o edition=unix`
meaning "detect" is confusing, so on the Linux side the option value is `auto`
(`-o edition=auto`), not `unix`.

### The justification is the removal, not the feature list

The in-tree `sysv` driver was removed by Jan Kara on 21 Feb 2025, merged for
6.15. The commit text is almost verbatim: "Since 2002 … the sysv filesystem was
doing IO under a rwlock in its `get_block()` function (yes, a non-sleepable lock
hold over a function used to read inode metadata for all reads and writes).
Nobody noticed until syzbot in 2023. This shows nobody is using the filesystem.
Just drop it." (Reviewed-by Jeff Layton and Darrick Wong, applied by Christian
Brauner.)

Two facts make the filsys case stronger than "we miss the old driver." First,
sysv covered exactly three formats, Xenix FS, SystemV/386 FS, and Coherent FS,
and all three are filsys editions, alongside V1-V10, 32V, 2.9/2.11BSD, and
PDP-7, so "replaces the removed driver and covers more" is literally true, not
rhetorical. Second, the specific defect is *structurally excluded* by the
backend shape already on the table: engine entry happens under a **sleepable**
lock (never a spinlock), with `filsys_bmap_ino` doing its I/O inside that. That
is a stated invariant, enforced by lockdep and `might_sleep()`, so the cover
letter can assert it and a reviewer can check it in thirty seconds.

The "why not FUSE?" question is sharper than it looks, and the honest answer
concedes capability. At the same LSFMM session, Jan Kara floated FUSE versions
of unwanted filesystems as the way to be rid of them, and filsys already *is*
that answer on Linux, shipping and CI-tested. So the cover letter cannot argue
capability: FUSE does everything except root-fs and `/proc/filesystems`
presence, and on Linux even root is reachable from an initramfs. The first
sentence has to be the removal, not the features: *the in-tree driver for these
formats was removed as unused and unsafe; this replaces it, default off, with
the lock bug structurally excluded and a test suite that already exists.* That
is an argument about the kernel's own history, and it is the only one that
survives the FUSE question.

The process bar is written down. `Documentation/filesystems/
adding-new-filesystems.rst` (Amir Goldstein, 21 May 2026, 195 lines, in 7.2)
came out of an LSFMM/BPF 2026 session whose stated motivation was that
unmaintained filesystems block VFS-wide work. It requires a `MAINTAINERS` entry
with `M:`, `L:` and `T:` lines, "strongly prefers" two or more maintainers, says
filesystems depending entirely on volunteer effort face higher scrutiny, and
says a track record in other subsystems strengthens the case.

### "Boring," by that standard

- **`fs/unixfs/` as `tristate`, default `n`.** Not built-in, not in defconfigs.
- **`fs_context` + `fs_parameter_spec` from day one.** No `->mount`.
- **Metadata I/O through the existing block path without holding a spinlock or
  rwlock across `submit_bio`**, the invariant above, enforced with lockdep and
  `might_sleep()`.
- **`memalloc_nofs_save()`/`memalloc_nofs_restore()` scoping, not `GFP_NOFS` at
  call sites.** Call-site `GFP_NOFS` has been discouraged since 4.12
  (`Documentation/core-api/gfp_mask-from-fs-io.rst`): the flags get missed and
  can't be audited. The Linux frontend wraps engine entry in the scope, and the
  Linux arm of `filsys_alloc` uses plain `GFP_KERNEL`, no allocation flag
  threaded through the eighteen sites, which is what the build-selected
  allocator exists to avoid.
- **No page cache in the first merge.** `generic_file_read_iter` is a page-cache
  path and needs a `read_folio`, which for a block filesystem means buffer heads
  or iomap, and `CONFIG_BUFFER_HEAD` is now optional and being phased out, so
  buffer heads are not the safe choice they used to be. The minimal honest
  option is a hand-written `->read_iter` that copies through `filsys_read_ino`
  into the iter with no page cache at all. Defensible for a read-mostly
  historical filesystem, but the costs must be named: no `mmap`, no readahead,
  and `->splice_read` needs its own answer. If `mmap` ever becomes wanted, go
  straight to iomap rather than through buffer heads.
- **The inode cache is the VFS inode cache**, keyed by `(sb, ino)`. No second
  handle table.
- **Userspace tools stay userspace**, `mkfs.unixfs`/`fsck.unixfs` already
  exist; `Documentation/filesystems/unixfs.rst` points at them.
- **KUnit or a fstests slice** that mounts a checked-in V7 image. Syzbot
  readiness is closer than it looks: `fuzz/filsys-fuzz.c` already fuzzes
  `filsys_detect` → `filsys_open_arch` → `filsys_invariants`, which is most of
  what syzbot does to a filesystem (mount a corrupt image and see what falls
  over). What is missing is the part after mount, driving `test_matrix`'s
  operation sequences over fuzzer-produced images rather than mkfs-produced
  ones, plus the allocation-failure arm. Both are userspace work, worth having
  regardless, and "here is the fuzz harness and here is what it covers" is
  unusually strong for a small filesystem.
- **One named maintainer** in `MAINTAINERS` (two is "strongly preferred"; one
  plus a public tree is what small fs actually ship with).

### Sequencing and the data path

Out-of-tree first is the engineering order, not a holding pattern: prove the
vnode/inode glue on OpenBSD/NetBSD, then wrap `inode_ops`/`file_ops`. Once the
module is dull, `modprobe unixfs`, `mount -t unixfs -o edition=v7 /dev/loop0
/mnt`, `ls` works, `fsck.unixfs` clean, posting to `linux-fsdevel` is an
ordinary review. This is the same shape as the OpenBSD advice: arrive with the
engine already proven and only the glue in question.

The data path follows §0's `bmap`, and the primitive is designed for iomap's
shape, not `get_block`'s: `iomap_begin` wants offset-and-length in and an extent
(offset, length, type) out, which is exactly what `filsys_bmap_ino` returns
(§0, refactoring 7). The one-block-at-a-time `get_block` callback is the wrong
shape to design for; the extent form serves iomap directly, and a `get_block`
callback can be derived from it if a consumer ever wants one.

## 8.4 plan9

plan9 has no callback VFS: a filesystem is a 9P
server, conventionally a userspace program attached with `9fs`/`mount`, or a
kernel `dev` driver, so this is the message family, and it needs none of the
kernel shims. The frontend maps the 9P request stream onto the node-anchored
core:

- `Twalk` (up to sixteen name elements against a fid) → a loop over
  `filsys_walk`;
- `Tread` on a directory (resumes at a byte offset) → the offset-resumable
  iterator;
- `Tstat`/`Twstat` → fill a `Dir` from the POSIX-free core.

The fid table is the open-handle lifetime, `Tclunk` releases the pin, which is
what §0's open-handle move leaves to the frontend. Do plan9 before QNX: the two
share the message-family mapping, and QNX's OCB is plan9's fid.

## 8.5 QNX

Not a kernel driver at all. QNX has no VFS: a filesystem is a **resource
manager**, a userspace process that registers a pathname prefix with `procmgr`
and answers `_IO_READ`/`_IO_WRITE`/`_IO_STAT`/`_IO_OPENFD` messages, usually via
the `iofunc_*` helpers. It is message-shaped, runs in userspace, uses ordinary
`malloc`, and needs none of the kernel shims, the same shape as plan9, which
is why it follows plan9: the OCB is the fid, and the message-family mapping is
largely already written. The one POSIX twist is that QNX genuinely wants
`struct stat`, so the FUSE filler is reused there rather than a new one written.

## 8.6 Haiku

Haiku is both a FUSE target and a native target. The native path is a
kernel filesystem add-on, a `file_system_module_info` module providing
`fs_volume_ops` and `fs_vnode_ops`, which is callback-shaped like the BSDs
(`lookup`, `read_dir` with an offset cookie, `read_stat` filling Haiku's
`struct stat`). The FUSE path is the `filsys` frontend under Haiku's FUSE 2.9.9
(§0, refactoring 5), running as a `userlandfs` module, since Haiku's FUSE
support is implemented through `userlandfs`. The add-on is `unixfs`; the FUSE
mount is `mount.unixfs`.
