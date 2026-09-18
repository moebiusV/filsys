# 0. Backend refactoring (first): one backend, four frontends

This section describes a refactor of libfilsys itself that **happens first**,
before the driver in §4–§6. It is a prerequisite: it reshapes the "as-is"
boundary (§4), resolves the plan's risks 1, 2, and 6, and supersedes §4.3 and
§4.4 by doing their work properly in the backend rather than as a kernel shim.

## TL;DR

libfilsys is the **backend** for four **frontends** — FUSE3, FUSE2, a native
OpenBSD kernel driver, and 9P — but its core is today shaped like a *single*
frontend (FUSE): the public API is path-based, POSIX-typed, whole-directory,
and it carries the open-handle table that only FUSE needs. The four frontends
demand four different shapes for the same four backend capabilities, so the fix
is to make the core **frontend-shaped** — node-anchored (resolves by inode
number, not path string), POSIX-free (returns plain integers, not `struct
stat`), offset-resumable (iterates a directory, does not slurp it), and
right-sized (allocates its caches per edition, not for the worst case) — and let
each frontend map itself onto it.

Net effect: roughly 400–500 lines out of the tree, ~3.6 KB off every mount, and
directory reads that stop allocating proportionally to directory size on all
four frontends at once.

## Measurements (HEAD)

| part | lines |
|---|---|
| engine (12 files) | 6,487 |
| headers | 1,572 |
| FUSE layer (7 files) | 1,008 |
| tools (mkfs/fsck/findfs/mount/detect/check) | 2,729 |

A few non-line measurements that shape the argument:

- `sizeof(filsys_edition_t)` is **4,296 bytes**, of which **3,984** is the
  free-block and free-inode caches.
- `filsys_dirent_t` is **66 bytes** for a format whose on-disk entry is 16.
- `fuseops.c`, `fuseops_macos.c`, and `fuseops_openbsd.c` are **549 lines**
  expressing three identical 25-entry vtables.
- The public API has **17 path-taking functions**.

## What the four frontends actually demand

The backend exposes a small set of capabilities; each frontend demands a
different shape for them. Where the columns differ is exactly what the backend
must stop assuming.

| demand | FUSE3 | FUSE2 | native kernel | 9P |
|---|---|---|---|---|
| name resolution | path string | path string | `(parent-vnode, name)` via `VOP_LOOKUP` | `(fid, name)` via `Twalk` |
| attribute fill | `struct stat` | `struct stat` | `struct vattr` | `Dir` |
| readdir resume token | index | index | `uio` cookie (byte) | byte offset into the marshalled stream |
| open-handle lifetime | engine-side table | engine-side table | `VOP_INACTIVE`/`VOP_RECLAIM` | `Tclunk` |

Two columns — FUSE3 and FUSE2 — are **identical**. They demand the same thing
from the backend on every axis that matters; their only differences (readdir's
flags argument, macOS's `setvolname`, OpenBSD's `getattr` without
`fuse_file_info` and `mknod` accepting `S_IFREG`) are adapter-level, not
backend-level. That is the argument for §5 stated as a table: the variation is
a quirk table, not a vtable.

The one column that is genuinely new — 9P — is the cheapest to satisfy once the
core is node-anchored and offset-resumable. `Twalk` carries up to sixteen name
elements against a fid, which is a loop over a node-anchored walk; `Tread` on a
directory resumes at a byte offset, which an offset-resumable iterator serves
directly. Building a path string just so the core can re-split it is work in
both directions, so the node-anchored core is what makes 9P cheap rather than
awkward.

## The refactorings

### 1. Node-anchored core

The 17 public path-taking functions are a thin shell over inode-anchored
internals, but today the shell lives *in the core*: `filsys.c` owns `split_path`
(21 lines), `resolve_parent`, and the path prologue of all 17 functions. Move
the shell out. The core becomes a single node-anchored primitive —

```c
int filsys_walk(filsys_t *fs, uint32_t ino, const char *name, uint32_t *out);
```

— and a small path helper moves into the FUSE frontend. FUSE calls the helper;
the kernel driver and 9P do not.

This is the change that makes 9P natural rather than a second path
re-implementation, and it is the one §4.3 was reaching for. Net: about 150
lines out of `filsys.c`, ~40 back into the FUSE frontend, and the ~200 lines
the plan would otherwise add never gets written.

### 2. POSIX-free core header

`filsys.h` pulls in `<sys/stat.h>`, `<sys/statvfs.h>`, `<time.h>`,
`<unistd.h>` — which is what forces the plan's risk 1 to choose between a
separate `filsys_kern.h` and `#ifdef FILSYS_KERNEL` guards. The core already
stores everything as plain integers in `filsys_inode_t` (84 bytes, all
`uint32_t`/`uint16_t`); export that plus a `filsys_statfs_t` of plain integers,
and let each frontend fill its own type — `struct stat` for FUSE, `struct
vattr` for the kernel, a `Dir` for 9P. `filsys_fill_stat` becomes FUSE's, not
the core's.

Count this one carefully: it is a **saving**, not a cost. The alternative is
risk 1's second option, an `#ifdef FILSYS_KERNEL` guard matrix sprayed across a
~300-line public header, plus a second public header that has to be kept in
sync. The saving is the guards *not written*, not the code added — the same
accounting the plan's risk 2 gets backwards when it says the `#define malloc`
"touches 0 call sites." Net LOC goes slightly up (≈30 out of the core, ≈25 each
into three frontends); net surface area goes sharply down. It removes the need
for `filsys_kern.h`, and it is what keeps `mode_to_posix` single-sourced instead
of re-implemented kernel-side.

### 3. One directory iterator, replacing `dir_read`

`VOP_READDIR` is offset-based and incremental; today the backend's only
whole-directory primitive is `dir_read`, which allocates the whole directory
and returns it as an array. `fuse_op_readdir` does this:

```c
int rc = filsys_readdir(c->fs, path, &ents, &count);
for (size_t i = 0; i < count; i++) { ... filsys_read_inode(...); emit(...); }
```

A 1,000-entry directory allocates a 66 KB array plus the raw directory buffer,
and reads 1,000 inodes, before emitting the first name. FUSE resumes by index,
so an interrupted readdir does it all again; 9P is worse, resuming at a byte
offset into the marshalled stream, which index-based resumption cannot map at
all without re-walking.

Replace it with one offset-resumable iterator:

```c
int filsys_dir_seek(filsys_t *fs, uint32_t ino, uint64_t off, filsys_iter_t *it);
int filsys_dir_next(filsys_iter_t *it, uint32_t *ino, const char **name,
                    uint16_t *namlen, uint64_t *next_off);
int filsys_dir_release(filsys_iter_t *it);
```

It serves the FUSE index, the kernel `uio` cookie, and 9P's byte offset — each
frontend computes its own resume token from `next_off`. It deletes both
`dir_read` implementations (40 lines in `dir_fixed.c`, 45 in `dir_bsd211.c`)
and deletes `filsys_dirent_t` from the hot path: the iterator hands back a
pointer into its buffer, so there is no 66-byte struct per entry at all.

Three contracts must be explicit. **Ownership:** the iterator owns a buffer up
to `V7_MAXBSIZE` (8192), which cannot sit on a kernel stack; in the kernel the
iterator holds the `struct buf *` from `bread`, points into `bp->b_data`, and
releases with `brelse` on `filsys_dir_release` — no copy, and the natural fit
for `VOP_READDIR`. **Termination:** on-disk names are fixed-width fields with no
NUL (V7/V6 are 14 bytes), so `*name` is *not* terminated and the caller must
use `*namlen`, not `strcmp`. **Lifetime:** the pointer is valid only until the
next `filsys_dir_next` or the release. The per-record machinery already exists —
`353578e` gave `dir_lookup`/`dir_add`/`dir_remove` chunked scans — and this is
the fourth caller of the same walk. It is the same change as the plan's risk 8:
the iterator removes the allocation rather than bounding it, and serves
`VOP_READDIR` in one move.

### 4. Right-size the allocator caches

`freelist_state` sizes `free[]` at `V8_NICFREE_LARGE = 946` because V9's 8K
superblock needs that depth. Actual depths: V7 50, 2.11BSD 50, Coherent 64, V6
100, Xenix 100 — thirteen of fifteen editions at 100 or below. Every mount pays
3,784 bytes for the worst case, plus 400 for the inode cache.

The descriptor already carries `.nicfree` and `.nicinod` per edition. Allocate
the two caches from those at mount, or tail-allocate them after the struct. A
V7 mount drops from 4,296 bytes to roughly 700. About 15 lines, no behaviour
change, testable entirely in userspace — and it answers the plan's risk 6 (the
in-kernel `pool(9)` sizing question) for free.

### 5. Fold the three FUSE adapters into one

`fuseops.c`, `fuseops_macos.c`, and `fuseops_openbsd.c` each define the same 25
vtable slots and 10–12 one-line wrappers. The real variation is small: FUSE3's
`readdir` takes a flags argument, macOS has `setvolname` and its own mount
options, OpenBSD's `getattr` has no `fuse_file_info` and its `mknod` has to
accept `S_IFREG`.

Keep the build-system selection already used for editions — `configure` picks
which `.c` links — but let it pick a small `const fuse_quirks_t` of behaviour
flags and option strings rather than a whole parallel adapter. One shared
adapter body reads the quirks. On the order of 300 lines gone, and it is the
same pattern the descriptor table already uses for the fifteen editions: data
varies, code doesn't.

Do **not** merge FUSE2 and FUSE3 into one file behind `#ifdef`. The
build-selected split is right; it is the triplication of the table that isn't.

### 6. Flatten the pointer chains (and hoist the loop calls)

The public layer reaches the per-edition machinery through two-hop chains —
`fs->ops->dir->dir_lookup(...)`, `fs->ops->inode->read_inode(...)` — and the
engine reaches the byte-order vtable through `fs->desc.bo->get16(...)` inside
loops the compiler cannot hoist (it can't prove the indirect callee doesn't
rewrite the pointer). `struct filsys` already caches `ops` (`filsys.c:326`);
extend that to the two leaves the public layer reaches:

```c
struct filsys {
    const struct filsys_ops       *ops;    /* already there */
    const struct filsys_dir_ops   *dir;    /* was fs->ops->dir   */
    const struct filsys_inode_ops *inode;  /* was fs->ops->inode */
};
```

Assigned once in `filsys_open`; `fs->ops->dir->dir_lookup(...)` becomes
`fs->dir->dir_lookup(...)` and reads better besides. Then hoist the loop calls
into a local — `v7fs.c:102` decodes up to 946 free-block entries and reloads
the `bo` pointer plus the `get16` slot every iteration; one hoist takes 1,892
loads to 2:

```c
uint16_t (*get16)(const uint8_t *) = fs->desc.bo->get16;
for (i = 0; i < nicfree; i++)
    fs->fl.free[i] = get16(sb + 6 + 2 * i);
```

Same move in the superblock free/inode cache decode and encode, the inode-table
walks, the checker's block-list decode, `dir_bsd211.c`'s record scan, and
`alloc_freelist.c`. The transport becomes a build-selected direct call (below),
so there is no `fs->io` pointer left to flatten. Leave the byte-order vtable a
vtable, and do not chase devirtualization via single-edition builds — the
flattening and the hoists are free and more readable; anything past them is
nanoseconds behind a disk read.

## The open-handle table moves to FUSE

`struct filsys` carries the open-handle table (`opens`, `nopen`, `nopen_cap`,
plus `filsys_open_ino`/`filsys_close_ino` and a `realloc`). It exists to pin an
inode across unlink-while-open. The kernel gets that free from
`VOP_INACTIVE`/`VOP_RECLAIM`, and 9P gets it free from `Tclunk`; only FUSE needs
it, because only FUSE has no handle lifetime the engine can see. Move it into
the FUSE frontend and the core loses a growable allocation and two public
functions.

## The allocator, concretely

Not a macro, and not a runtime vtable. The allocator doesn't vary at runtime —
there is exactly one per build — so it follows the existing build-selection
pattern: `configure` picks `filsys_alloc_user.c` or `filsys_alloc_kern.c`, and
the engine calls two direct functions:

```c
void *filsys_alloc(filsys_alloc_kind_t kind, size_t n, int zero);
void  filsys_free(filsys_alloc_kind_t kind, void *p, size_t n);

typedef enum {
    FILSYS_AL_MOUNT,    /* filsys_edition_t + its right-sized caches */
    FILSYS_AL_BLOCK,    /* one logical block, <= V7_MAXBSIZE */
    FILSYS_AL_BLKLIST,  /* blocktree's growable block vector */
    FILSYS_AL_SCRATCH,  /* short-lived, variable */
} filsys_alloc_kind_t;
```

The enum is what lets the kernel arm route the fixed-size hot kinds —
`FILSYS_AL_BLOCK` and `FILSYS_AL_MOUNT` — to a `pool(9)` each, and the rest to
`km_alloc(M_FILSYS)`. The engine never learns pools exist; in the userspace arm
both kinds are `malloc`/`calloc` and `free`, and `kind` is ignored.

Two properties that matter in the kernel:

- **`kd_nowait`, not `kd_waitok`.** The engine already returns `-ENOMEM` in
  sixteen places (5 in `blocktree.c`, 4 in `filsys.c`, 2 each in the directory
  codecs, and so on), so the kernel arm can use `kd_nowait` and let the failure
  propagate as the engine already expects. That dissolves risk 8's hang rather
  than managing it — a `#define malloc km_alloc(..., kd_waitok)` would bake the
  sleeping variant in at every site, and `kd_waitok` under the per-mount
  exclusive lock would stall every other operation on that mount until the
  pagedaemon freed something.
- **No `realloc`.** The OpenBSD kernel has none, so the pair is alloc/free
  only. After the open-handle table moves to FUSE, `filsys.c:424` goes with it
  and the core has one `realloc` left — `blklist_push` at `blocktree.c:149` —
  which becomes alloc-copy-free in about six lines. Doubling from 128
  `uint32_t`s that is a few KB of transient peak at worst, and the failure
  semantics are the same: on a failed alloc you still hold the old array and
  return `-ENOMEM`, which is what `realloc` returning NULL gives you.

`filsys_free` taking the size is not decoration — `km_free(9)` requires it, so
the signature carries it from the start or every call site gets touched twice.

Keep a settable override next to `filsys_alloc` — an inject arm beside the
production arm, exactly as the transport does below — purely so the test build
can inject allocation failures. Sixteen call sites gain a parameter they already
have in scope.

## The transport, too: build-selected

`filsys_set_io` has nine call sites and every one is in `test_matrix.c` — zero
production users. The transport is build-time known in every shipping
configuration (`filsys_io_file` in userspace, `filsys_io_kern` in the kernel),
so by the same rule as the allocator it should be a build-selected direct call,
not a pointer:

```
filsys_io_file.c     production, userspace   -> direct call
filsys_io_kern.c     production, kernel      -> direct call
filsys_io_inject.c   --enable-fault-injection, default off
```

The engine calls `filsys_read_bytes(fs, buf, n, off)` unconditionally; which
object supplies it is a link-time question. `filsys_io_t`, `filsys_set_io`, and
the `io` member of `filsys_edition_t` disappear from the production build.

The win is not speed — fourteen indirect calls behind a `pread`/`bread` are
unmeasurable. It is two other things. **Hardening:** a writable function-pointer
pair reachable from every block read is a redirect target, and in a kernel
driver that is a live concern OpenBSD treats as one; a direct call has nothing
to overwrite. **Shipped API surface:** `filsys_set_io` is "Internal, not part of
the public filsys.h API" only as a comment; removing it from production makes
that real, and it is one fewer knob for the 9P server and the kernel driver to
reason about.

The cost is that the tested binary is not quite the shipped binary. It is
containable because the divergence is only in *how the leaf is reached*, not
what it does — `filsys_io_inject.c` calls the same `filsys_read_bytes` body when
unarmed, so the format code under test is byte-identical. Build both in CI and
run the matrix against each; the fault-injection build must stay first-class,
since it produced the dangling-entry reproduction and the slow suite is already
skipped by default. (`filsys_invariants` stays as-is: the fuzzer calls it and it
is the same walk `filsys_check` drives, not a transport concern.)

## Testing: keep it in userspace

`fuzz/filsys-fuzz.c` is a libFuzzer harness over `filsys_detect` →
`filsys_open_arch` → `filsys_invariants`, and `instrument.c` has five
file-scope globals. Neither belongs in a kernel build, and neither needs to be
there — they exercise the format engine, which is byte-identical in both
builds. That is what makes §7's "transport parity first" load-bearing rather
than a nicety: run `test_matrix` and the fuzzer against a userspace transport
written to the same sub-block-assembly rules as `filsys_io_kern`, and the only
thing left unproven in the VM is the vfs/vnode glue.

Two things that harness should cover which it doesn't today, both kernel-
specific and still userspace-testable: a transport whose `read`/`write` are
`DEV_BSIZE`-granular with unaligned head/tail assembly, and an allocator arm
that fails on the *n*th allocation. The second is worth having regardless —
sixteen `-ENOMEM` paths and nothing currently exercises them.

The QEMU loop, then, is for the glue only. `vnd(4)` over a file, a serial
console, and a qcow2 snapshot you roll back after each panic; `ddb` on the
console is usable, and `boot -c`/`config -e` toggles the option without
rebuilding. The thing that makes it painful is treating the VM as the
edit-compile-test loop; the thing that makes it tolerable is arriving with the
engine already proven and only the glue in question. Budget for the first
several boots ending in `ddb` on a NULL `vop_` slot — which is the practical
argument for filling `vop_abortop`, `vop_pathconf`, `vop_print` and friends in
Phase 1 rather than discovering them one panic at a time.

## Rough totals

Changes 1, 3, 5, and the open-handle move are each net-negative on lines; 2 is
net-positive by the literal count but net-negative once the guards it deletes
are counted; 4 is about neutral. The estimate is **400–500 lines out** of the
tree, against a starting point of ~11,800 excluding tests. The memory side is
more decisive: per-mount footprint drops by roughly 3.6 KB, and directory reads
stop allocating proportional to directory size on every frontend at once.

## What to leave alone

The edition descriptor table, the allocator and directory-format vtables, and
the byte-order vtable are all carrying real variation and are already the right
shape — they are why this port is tractable at all. The two build-selected
seams — allocation and I/O — carry the per-build variation (userspace vs
kernel); the I/O one still has to carry the device size alongside the
byte-offset read/write, as §4.2 says.

## How this revises the rest of this plan

This refactor precedes and simplifies the driver, and it resolves three of the
plan's open risks rather than competing with them:

- **Risk 1 (public-header boundary)** is gone: a POSIX-free `filsys.h` means
  there is no `filsys_kern.h` and no `#ifdef FILSYS_KERNEL` matrix.
- **Risk 2 (`malloc` shimming)** is gone: after the core is node-anchored and
  POSIX-free, the eighteen allocation sites are the last libc dependency
  standing, `fs` is already in scope at fifteen of them. Of the two bootstrap
  `calloc`s in `filsys.c`, one allocates the `filsys_t` wrapper (which the
  kernel frontend replaces with `struct filsys_mount`) and one allocates
  `fmt.state_size` — the `filsys_edition_t` itself, which the kernel still needs
  and which is exactly `FILSYS_AL_MOUNT`. So it is one replaced, one converted.
  The build-selected `filsys_alloc` is one arm per build, not a fork — and no
  `#define`.
- **Risk 6 (allocator union sizing)** is answered by §4 (right-size the caches):
  the caches become per-mount, per-edition, which is both the userspace saving
  and the kernel `pool(9)` sizing answer.

And §4.3 (path-based mutation) and §4.4 (readdir) are superseded by §1 and §3
here respectively — they were describing the same work as a risk, where this
section does it as the plan.
