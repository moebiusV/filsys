# FUSE handle-fidelity conformance

filsys after 1.9.3 anchors its deferred-free lifecycle on `fi->fh`: `read`,
`write`, `getattr`, `truncate` and `release` resolve identity through the open
handle, and the hard_remove free waits for `release` to arrive once per handle.
That assumption is the riskiest one in the codebase, and it lands on the one
thing that differs most across FUSE implementations.  This probe measures it.

`conformance/fuse-conformance.c` is a standalone minimal FUSE filesystem (no
filsys dependency) serving one file `/f`; it logs, per callback, whether `fi`
was passed and what `fi->fh` carried, and assigns handles as an incrementing
counter so the transcript shows whether the kernel echoed back the exact value
`open` returned.  `conformance/conformance-client.c` drives the torture sequence
and correlates each step with the callbacks it produced.  `conformance/run.sh`
mounts, drives, unmounts, and reports.

Run it with `sh conformance/run.sh`.  The six questions:

1. does `getattr` receive `fi`, and is `fi->fh` what `open` returned?
2. same for `truncate`?
3. does `release` fire at last `close(2)`, or later at vnode reclaim?
4. is `open` deduplicated per (vnode, mode), and do `open`/`release` balance?
5. do two processes holding one file get distinct `fh` values?
6. does `unlink` of an open file reach the filesystem (hard_remove), or is it
   silly-renamed by libfuse?

## Reference transcript — Linux (fuse3)

```
== open(O_RDWR|CREAT), fstat(fd)
    fs: INIT hard_remove=1
    fs: GETATTR /f fi=0
    fs: OPEN /f flags=0x8002
== ftruncate(fd, 100) on the open fd
    fs: TRUNCATE /f size=100 fi=1 fh=1
    fs: GETATTR /f fi=1 fh=1
== open(O_RDONLY), close(fd)
    fs: FLUSH /f fi=1 fh=1
    fs: RELEASE /f fi=1 fh=1
    fs: OPEN /f flags=0x8000
    fs: FLUSH /f fi=1 fh=2
== two concurrent opens (fd1, fd2), close both
    fs: RELEASE /f fi=1 fh=2
    fs: OPEN /f flags=0x8000
    fs: OPEN /f flags=0x8000
    fs: FLUSH /f fi=1 fh=3
    fs: RELEASE /f fi=1 fh=3
    fs: FLUSH /f fi=1 fh=4
== parent + child each open the file
    fs: RELEASE /f fi=1 fh=4
    fs: OPEN /f flags=0x8000
    fs: FLUSH /f fi=1 fh=5
    fs: OPEN /f flags=0x8000
    fs: FLUSH /f fi=1 fh=6
    fs: RELEASE /f fi=1 fh=6
    fs: FLUSH /f fi=1 fh=5
    fs: RELEASE /f fi=1 fh=5
== open(O_RDWR), unlink(path) while open, close(fd)
    fs: OPEN /f flags=0x8002
    fs: UNLINK /f
    fs: FLUSH (null) fi=1 fh=7
    fs: RELEASE (null) fi=1 fh=7
```

Answers on Linux: **1** yes (`GETATTR … fi=1 fh=1`), **2** yes
(`TRUNCATE … fi=1 fh=1`), **3** release at close (every open is followed by a
matching `RELEASE`, nothing deferred to unmount), **4** no dedup (distinct
`fh` per open, and `open`/`release` balance one-to-one), **5** yes (parent
`fh=5`, child `fh=6`), **6** hard_remove (a plain `UNLINK`, and the post-unlink
`FLUSH`/`RELEASE` show the null path while `fi`/`fh` are still intact).

## Platform table

| Platform | getattr fi | truncate fi | release timing | open dedup | 2-proc fh | unlink-of-open |
|----------|------------|-------------|----------------|------------|-----------|----------------|
| Linux (fuse3) | yes | yes | close | no | distinct | hard_remove |
| FreeBSD 15.1 (fuse3) | no | yes | close | no | distinct | hard_remove |
| DragonFly 6.4.2 (fuse3) | no | **no** | close | no | distinct | hard_remove |
| OpenBSD (fuse2) | —* | —* | close† | — | — | hard_remove |
| NetBSD (fuse3) | — | — | — | — | — | — |
| HardenedBSD | — | — | — | — | — | — |
| MidnightBSD | — | — | — | — | — | — |
| illumos (libfuse 2.7.6) | — | — | — | — | — | — |

`—` means not yet measured.  `*` FUSE2 declares `getattr`/`truncate` without a
`fi` parameter, so those two answers are structural rather than measured.  `†`
from the `finding-a` test (2000 files created and re-opened, zero `ENFILE`,
`statfs` recovered): release fires at last close on OpenBSD, not at vnode
reclaim.

## DragonFly: `truncate` gets no `fi`

DragonFly 6.4.2's fusefs does **not** pass `fi` to `truncate`
(`TRUNCATE /f size=100 fi=0`), even though FreeBSD 15.1 — its ancestor — does
(`fi=1 fh=1`).  This is the same shape as FUSE2: without `fi` there is no
`fi->fh` to reach the inode by, so `ftruncate(2)` on an unlinked-but-still-open
descriptor cannot be routed by the handle and returns `ENOENT` there, exactly as
on OpenBSD.  (On Linux and FreeBSD it works; see `test_fuse_unlink_open`.)  The
rest of the deferred-free lifecycle is unaffected: `release` fires at close, and
`open`/`release` balance one-to-one, so hard_remove frees still land correctly.

## CI results (run 34415278140)

Probe transcripts recorded 2026-09-09:

- **FreeBSD 15.1** — `getattr` carries no `fi` (`fi=0`), but `truncate` does
  (`fi=1 fh=1`); release at close; no dedup; two-process `fh` distinct;
  unlink-of-open is a plain `UNLINK` (hard_remove).
- **DragonFly 6.4.2** — as FreeBSD except `truncate` has `fi=0` (see above).
- **OpenBSD 7.9** — the probe binary did not compile under the first CI
  (a bare `cc -lfuse` cannot see base `fuse.h`; fixed to `pkg-config --cflags
  --libs fuse`).  The `finding-a` end-to-end test still passed.
- **NetBSD / MidnightBSD / HardenedBSD** — the job failed in `prepare`, before
  the probe ran: NetBSD's package mirror returned "no pkg found for curl";
  MidnightBSD's VM had no `pkg` tool (`sh: pkg: not found`); HardenedBSD's
  integrity hardening refused to run the freshly installed binaries (`Tainted
  process refusing to run binary`).  All three are VM-image/package-manager
  issues, not filsys.
