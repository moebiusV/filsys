# retro-fuse comparison

filsys is a clean-room reimplementation of the Research Unix filesystems.
[jaylogue/retro-fuse](https://github.com/jaylogue/retro-fuse) is the opposite
approach: it incorporates the *original* V6/V7/2.9BSD/2.11BSD kernel source,
"lightly modernized" to compile and run in userspace.  Running retro-fuse's
genuine code through filsys's own tests lets us record, concretely, how a
modern filesystem API differs from the genuine Unix code it reimplements — the
operations the ancient kernels could not express, and the ones they silently
got wrong.

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

Built retro-fuse's FUSE-free V7 core (`v7fs.c` + `v7adapt.c` + `idmap.c` +
`dskio.c` + `ancient-src/v7/*`), drove it through filsys's boundary matrix, and
checked the result with filsys's own `fsck.filsys` — the same checker filsys's
suite uses, so "clean" means the same thing for both.

| Probe | retro-fuse V7 | filsys |
|---|---|---|
| writes at 5120 / 5121 / 1 / 511 / 136192 / 200000 / 1 MiB | all round-trip | all round-trip |
| fsck after write + unlink | `errors=0 missing=0 dup=0` | `errors=0 missing=0 dup=0` |
| truncate to 0 | works | works |
| truncate to a non-zero size | **fails** (`-EINVAL`) | works |
| disk-write failure surfaced to caller | **swallowed** (write "succeeds") | propagates `-EIO` |

The boundary writes and the allocator are sound in both — the genuine V6/V7
write path is correct.  The differences are semantic, not structural.

## Differences: what genuine Unix code does that filsys fixed

### 1. `truncate` — genuine V7 is truncate-to-zero only

`v7fs_truncate` (retro-fuse) is a faithful port of V7's `trunc(2)`:

```c
if (length == 0) { v7_itrunc(ip); res = -v7_u.u_error; }
else             res = -EINVAL;   /* any non-zero length is rejected */
```

V7 had no `ftruncate`: the only operation was "truncate a file to zero", which
`itrunc` implements.  Arbitrary-length truncation (shrink to 5, grow to 300000)
returns `EINVAL`.  filsys implements the full down/up/zero contract on the same
on-disk format (free the tail blocks via `itrunc`-style traversal for shrink,
allocate sparsely for grow).

### 2. Write errors are swallowed — genuine V6/V7 `write(2)` is fire-and-forget

`dsk_write` (retro-fuse's disk layer) returns `-EIO` on a failed `pwrite`, and
the adapter sets `B_ERROR` on the buffer.  But V7's `bwrite` returns `void` and
`writei` does not check `B_ERROR` after issuing the write, so the error never
reaches `v7fs_pwrite`: the caller is told the write succeeded even though the
block was not persisted.

Measured directly: with a fault injected at every one of the 396 `dsk_write`
calls a 200000-byte write makes, `v7fs_pwrite` returned success all 396 times.
This is genuine V7 behavior (errors surfaced asynchronously via `B_ERROR` on a
*later* read, not synchronously to the writer).

filsys's contract is the opposite, and it is load-bearing for its tests:
`filsys_write` returns `-EIO` immediately, and `filsys_close` returns the final
flush result — "never return 0 if any step failed; flush error wins".  A caller
on filsys can *tell* a write did not land; a caller on retro-fuse cannot.

### 3. `open(O_CREAT)` — a retro-fuse adapter bug (not V7's)

`v7fs_open` with `O_CREAT` segfaults: it calls `v7_namei(..., 0)` then
`v7_maknode`, but `maknode` dereferences `u.u_pdir`, which `namei` only sets
when called with `flag=1`.  `v7fs_mknod` does it correctly (`namei(..., 1)`);
`v7fs_open`'s inlined create path does not.  This is a defect in the adapter
layer, not a faithful V7 quirk — the original kernel routes `O_CREAT` through
`mknod` first.

## Fault injection

filsys's fault injection is a ~20-line seam swap (`filsys_set_io`).  The
analogous seam in retro-fuse is `dsk_read`/`dsk_write`, which is **shared by all
four editions**, so one change covers them all.  The port is trivial.

But the result is vacuous on retro-fuse: because of finding 2, an injected
`-EIO` cannot be observed through the API — `v7fs_pwrite` still reports
success.  filsys's crash-consistency assertion ("`dup == 0` at every fault
position") is meaningful only because filsys *surfaces* the fault and then must
unwind to a consistent state.  On retro-fuse you can inject the fault but you
cannot tell the difference from the outside; the aliasing invariant holds
trivially because the failed write is silently dropped, never partially applied.

This is the cleanest single illustration of the gap: **filsys's tests assume a
filesystem that reports its own failures; the genuine V6/V7 code predates that
assumption.**

## Bottom line

| | filsys | retro-fuse (genuine V6/V7) |
|---|---|---|
| on-disk fidelity | reimplemented, verified against originals | the originals |
| API surface | one uniform `filsys_*` across 15 editions | four separate `*fs_*` APIs |
| truncate | down / up / zero | zero only |
| write-failure visibility | synchronous `-EIO` + flush result from `close` | silent |
| fault injection | meaningful (surface + unwind) | injectable but unobservable |
