# 4. What "using libfilsys as-is" means, precisely

The phrase needs to be nailed down, because "link the userspace library into
the kernel" is impossible (no libc, no `struct statvfs`, no `open(2)`). The
workable and honest reading is:

> **The on-disk format engine is compiled into the kernel byte-for-byte
> unchanged; only the userspace convenience layer is replaced by a thin
> kernel-side shim.**

Concretely, the boundary splits libfilsys into two halves.

## 4.1 The engine half — compiled as-is

| file | what it is | kernel status |
|---|---|---|
| `v7fs.c` | V7/V8-family/32V/Coherent/Xenix/2.9BSD/System III–V codec, superblock codec, `filsys_io_file` | **as-is** (the `filsys_io_file` *definition* may be `#ifdef`'d out; the driver supplies its own `filsys_io_t`) |
| `v1fs.c` | V1/V2/V3 codec | as-is |
| `pdp7fs.c` | PDP-7 word-addressed codec + `rb09`/`packed18`/`rim` | as-is |
| `alloc_freelist.c` | V6/V7 free-list allocator | as-is |
| `alloc_v8bitmap.c` | V8-family bitmap allocator | as-is |
| `blocktree.c` | shared indirect-block walker | as-is |
| `dir_fixed.c` / `dir_bsd211.c` | directory codecs | as-is |
| `byteorder.c` | `bo_le`/`bo_be`/`bo_me` vtable | as-is |
| `filsys_format.c` / `filsys_names.c` | edition table + name/alias resolution | as-is (or trimmed to the runtime subset) |
| `filsys.c` | runtime open/read/write/lookup/create/rename/… + the shared `filsys_ops` router | runtime subset as-is |

The kernel build **omits** `filsys_mkfs.c`, `filsys_detect.c`, `check.c`, and
the findfs/fsck drivers (they are offline tools, §1.3), and omits the FUSE
adapter (`fuseops*.c`, `fuse_core.c`). This is the same split the FUSE-free
`test_matrix` build already makes.

One caveat to "as-is": the codecs open and size their own backing store —
`open()`/`close()` and `filsys_dev_size(fs->fd, …)`, which `fstat`s an fd
(`filsys.h:27`, at `v7fs.c:180`, `v1fs.c:66`, `pdp7fs.c:186`). The kernel has
no fd, so those calls are excluded (or routed through the transport); the mount
entry point carries the device size in via `DIOCGDINFO` instead (§4.2).

## 4.2 The shim half — new, thin, kernel-only

1. **`filsys_io_kern`** — a `filsys_io_t` whose `read`/`write` translate a
   byte offset + length into `bread`/`getblk`/`VOP_STRATEGY` on the block
   device vnode (§5.3 in [05-architecture.md]). This is the single most
   important piece, and it is the reason the codecs need no change. It also
   carries the device **size**: the codecs' `open`/`close`/`filsys_dev_size`
   are excluded, and the mount entry point supplies the size from `DIOCGDINFO`
   on the partition — the transport seam is size + I/O vtable, not the vtable
   alone.
2. **Allocation shim** — map `malloc`/`calloc`/`realloc`/`free` (≈27 alloc /
   285 free call sites in the engine) onto the kernel allocator
   (`km_alloc`/`km_free` with a dedicated `M_FILSYS` type, or `pool(9)` for the
   hot fixed-size `filsys_inode_t`/`filsys_dirent_t`/block buffers). Prefer a
   small `unixfs_kern.h` with wrappers over `#define malloc …` (§5.4).
3. **Logging shim** — `printf`/`fprintf`/`snprintf`/`vsnprintf` → kernel
   `printf`/`snprintf`. The interactive `filsys_query()` (`getchar` on stdin)
   is *not* ported: it lives in `check.c`, which is excluded.
4. **Stat/time shim** — `struct stat`/`struct statvfs`/`struct timespec` from
   the public header are userspace-only. The driver decodes the already-typed
   `filsys_inode_t` (plain `uint32_t` fields) directly into `struct vattr`
   and `struct statfs`; times come from `nanotime(9)` (§5.6).

## 4.3 The frontend maps its semantics to the backend

The public mutation API takes **paths** (`filsys_create(path, …)`), but the
kernel VFS hands the driver **`(parent-vnode, name)`**. This is the ordinary
frontend-to-backend mapping, not a gap: the engine does the correct thing
(inode-anchored `dir_lookup`, `dir_add`, `dir_remove`, `ialloc`, `ifree`,
`bmap`, `read_inode`, `write_inode`), and the frontend maps its own semantics
onto those primitives — the VFS frontend maps `(parent-vnode, name)` to
`(parent-inode, name)`, just as the FUSE frontend maps path strings today. The
path-based functions are thin wrappers over those internals. Two options:

- **(Preferred) Export the inode-anchored primitives.** Add a small additive,
  behavior-preserving internal API — e.g. `filsys_dir_lookup_in(fs,
  parent_ino, name, &ino)`, `filsys_create_in(fs, parent_ino, name, mode, uid,
  gid, &ino)`, `filsys_unlink_in(fs, parent_ino, name)`, etc. — by hoisting the
  existing static helpers. This is *not* a rewrite; it is making the seam that
  already exists visible to a second caller (the kernel driver, beside FUSE).
- (Alternative) Have the driver maintain parent pointers and synthesize paths;
  rejected as fragile and wasteful.

## 4.4 Directory iteration (readdir)

The kernel `VOP_READDIR` is incremental and offset-based; the backend's only
whole-directory primitive is `dir_read`, which allocates the whole directory
and returns it as an array (`filsys_dir_ops`). A `VOP_READDIR` wants one entry
per call, so the engine gains a `readdir`-style iterator — advance-by-offset
over the directory records, which the per-record walks inside `dir_lookup`/
`dir_add`/`dir_remove` already embody — and the frontend streams entries. Where
an edition has no natural stream (V1's fixed 10-byte slots, PDP-7's
word-addressed entries), the backend synthesises a stable index and the
frontend maps to that.
