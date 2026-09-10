# Grading retro-fuse against filsys's test suite

filsys is a clean-room reimplementation of the Research Unix filesystems.
[jaylogue/retro-fuse](https://github.com/jaylogue/retro-fuse) is the opposite
approach: it incorporates the *original* V6/V7/2.9BSD/2.11BSD kernel source,
"lightly modernized" to compile and run in userspace.  Running retro-fuse's
genuine code through filsys's own tests records, concretely, how a modern
filesystem API differs from the genuine Unix code it reimplements.

This document has two waves.  **Wave 1** (complete) grades what filsys's test
suite can measure from the outside: what retro-fuse passes, what it fails, and
what it cannot be graded on without instrumentation.  **Wave 2** (pending)
documents what deeper probes — inserted into retro-fuse's own code — reveal
about the tests Wave 1 could not reach.

## What retro-fuse is

- Four distinct on-disk formats: **V6** (also V4/V5), **V7**, **2.9BSD**,
  **2.11BSD**.
- Per-edition layout: `ancient-src/<ed>` (original kernel `.c`), plus
  `src/<ed>adapt.c` (glue), `src/<ed>fs.c` (modern C API), `src/<ed>fuse.c`
  (FUSE2, `FUSE_USE_VERSION=26`).
- **Four separate C APIs** (`v6fs_*`, `v7fs_*`, `bsd29fs_*`, `bsd211fs_*`) — no
  unified ops vtable, unlike filsys's single `filsys_edition_t` + `filsys_*`.
- One shared I/O seam: `dsk_read`/`dsk_write` in `src/dskio.c`.
- No in-process fsck — its test suite runs the original `icheck`/`dcheck` under
  SIMH as an oracle.

## Method

Built retro-fuse's FUSE-free **V7** core (`v7fs.c` + `v7adapt.c` + `idmap.c` +
`dskio.c` + `ancient-src/v7/*`), drove it through filsys's operation matrix, and
checked the result with filsys's own `fsck.filsys` — the same checker filsys's
suite uses, so "clean" means the same thing for both.  filsys's fault injection
was ported to retro-fuse's `dskio` seam as a ~20-line `dsk_fault_arm(mode, n)`.

# Wave 1 — complete

## Grading summary

| test | verdict |
|---|---|
| boundary writes (5120 / 5121 / 1 / 511 / 136192 / 200000 / 1 MiB) | **pass** |
| allocator integrity (fsck after write + unlink) | **pass** (`errors=0 missing=0 dup=0`) |
| truncate to 0 | **pass** |
| truncate to a non-zero size | **fail** (`-EINVAL`) |
| `open(O_CREAT)` | **fail** (SIGSEGV) |
| single-fault injection → `dup == 0` at every position | **ungraded** (fault swallowed) |

## Passes

**Boundary writes** round-trip at every size that straddles a structural
boundary: the direct-block capacity (5120 = 10×512), one byte past it (5121,
forcing single-indirect), the odd-byte and half-block cases (1, 511), the
single-indirect ceiling (136192), double-indirect (200000), and 1 MiB.  The
genuine V7 write/read path is correct at every one.

**Allocator integrity** is clean: after writing a 1 MiB file and unlinking it,
filsys's fsck reports `errors=0 missing=0 dup=0` — every block the write
allocated was freed on unlink, no leak, no aliasing.

## Failures

### 1. `truncate` to a non-zero size — `-EINVAL`

`v7fs_truncate` is a faithful port of V7's `trunc(2)`:

```c
if (length == 0) { v7_itrunc(ip); res = -v7_u.u_error; }
else             res = -EINVAL;   /* any non-zero length is rejected */
```

V7 had no `ftruncate`: the only operation was "truncate a file to zero".
Arbitrary-length truncation (shrink to 5, grow to 300000) returns `EINVAL`.
filsys implements the full down/up/zero contract on the same on-disk format
(free the tail blocks for shrink, allocate sparsely for grow).  This is a
**genuine V7 limitation**, faithfully reproduced — the grade is "fails the
modern-truncate test", not "buggy for its own contract".

### 2. `open(O_CREAT)` — SIGSEGV (a retro-fuse adapter bug, not V7's)

`v7fs_open` with `O_CREAT` segfaults: it calls `v7_namei(..., 0)` then
`v7_maknode`, but `maknode` dereferences `u.u_pdir`, which `namei` only sets
when called with `flag=1`.  `v7fs_mknod` does it correctly (`namei(..., 1)`);
`v7fs_open`'s inlined create path does not.  The original kernel routes
`O_CREAT` through `mknod` first, so this is a defect in the adapter layer, not a
faithful V7 quirk.

## Ungraded — and why

### Fault injection (`dup == 0` at every fault position) — vacuous

filsys's fault-mutation test asserts: after a single injected I/O failure, the
filesystem unwinds to a state where no block is both free and referenced
(`dup == 0`), and salvage recovers the rest.  It grades the **allocator's
write-ordering** under partial failure.

The injection itself reaches retro-fuse's disk layer: `dsk_write` returns
`-EIO`, the adapter sets `B_ERROR` on the buffer, and the write count is
observable (a 200000-byte write issues 396 `dsk_write` calls).  But the fault
never reaches the caller: V7's `bwrite` returns `void` and `writei` does not
check `B_ERROR`, so `v7fs_pwrite` reported **success at all 396 fault
positions**.  The test's premise — "the operation fails and must unwind" — never
holds, so there is nothing to grade.  The `dup == 0` result is trivially true
because the failed block is silently *dropped*, never partially applied.

**Why this is the whole story in one line:** filsys's tests assume a filesystem
that reports its own failures; the genuine V6/V7 code predates that assumption.

# Wave 2 — pending

The tests below are reachable only with probes inserted deep enough into
retro-fuse's own code.  Findings go here as they land.

- **Crash-prefix enumeration** (kill the process after *k* of the *W*
  `dsk_write`s, fsck the partial image, require `dup == 0` at every prefix).
  Unlike fault injection this is gradeable — the process death leaves an
  observable prefix state — but needs a fork-per-prefix driver and the
  write-count hook (`dsk_fault_count` already provides it).  This is the
  probe that will actually answer "is retro-fuse's allocator crash-consistent".
- **Read-path fault injection** — whether V7's `bread` propagates an injected
  `-EIO` to the caller (it may, unlike `writei`); not yet measured.
- **`rename` / `rmdir` / `mkdir` / `link` / `chmod` semantics** — the modern
  `rename(2)` contract filsys implements on top of the format; not yet driven
  against retro-fuse's genuine `nami`/`sys3` code.
- **V6 / 2.9BSD / 2.11BSD editions** — Wave 1 drove V7 only; the other three
  APIs are separate and need their own drivers (the "no unified vtable"
  friction).  V6 is expected to reproduce the same truncate-zero-only and
  swallowed-write behavior (same kernel lineage); 2.9/2.11BSD's 4-byte block
  addresses and variable-length directories are a fresh set of boundaries.
- **Property-based sequences** — filsys's random-op-history + model; needs a
  harness over the per-edition API.
