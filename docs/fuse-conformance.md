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

Run it with `sh conformance/run.sh`.  The seven questions:

1. does `getattr` receive `fi`, and is `fi->fh` what `open` returned?
2. same for `truncate`?
3. does `release` fire at last `close(2)`, or later at vnode reclaim?
4. is `open` deduplicated per (vnode, mode), and do `open`/`release` balance?
5. do two processes holding one file get distinct `fh` values?
6. does `unlink` of an open file reach the filesystem (hard_remove), or is it
   silly-renamed by libfuse?
7. does `truncate` carry `fi->fh` to an **unlinked** open fd (the `tmpfile`
   shape the lifecycle needs for `ftruncate`)?

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

Question 2 measures `truncate fi` on a **linked** open fd; question 7 measures
the same on an **unlinked** fd.  The lifecycle's actual requirement is Q7:
`fi->fh` must reach `truncate` on an unlinked fd, or `ftruncate(2)` after unlink
falls through to the (gone) path and returns `ENOENT`.  `test_fuse_unlink_open`
cross-checks the same thing on the real filsys mount (where only filsys knows
the inode is still allocated).  `getattr fi` likewise means "on the open
(fstat) fd".

| Platform | getattr fi | truncate fi (linked) | truncate fi (unlinked) | release | unlink-of-open |
|----------|------------|----------------------|------------------------|---------|----------------|
| Linux (fuse3) | yes | yes | yes | close | hard_remove |
| FreeBSD 15.1 | yes | yes | yes | close | hard_remove |
| HardenedBSD | yes | yes | yes | close | hard_remove |
| MidnightBSD | yes | yes | yes | close | hard_remove |
| DragonFly 6.4.2 | no | **no** | **no** | close | hard_remove |
| NetBSD 11.0 (librefuse) | no | yes | **no** | close | hard_remove |
| OpenBSD 7.9 (fuse2) | no* | no* | **no** | close† | hard_remove |
| illumos (libfuse 2.7.6) | — | — | — | — | — |

`—` means not yet measured.  `*` FUSE2 declares `getattr`/`truncate` without a
`fi` parameter, so those two answers are structural rather than measured.  `†`
from the `finding-a` test (2000 files created and re-opened, zero `ENFILE`,
`statfs` recovered): release fires at last close on OpenBSD, not at vnode
reclaim.

The deferred-free lifecycle itself (unlink-while-open keeps the inode until
`release`, then frees it) holds on **every** platform: `open`/`release` balance
one-to-one, `release` fires at close, and unlink-of-open is a plain `UNLINK`
(hard_remove) everywhere.  The `truncate fi (unlinked)` column is only the
`ftruncate`-on-an-unlinked-fd path — the `tmpfile(3)` shape — which works on
Linux, FreeBSD, HardenedBSD and MidnightBSD, and does not on DragonFly, NetBSD
or OpenBSD.

## DragonFly: `truncate` gets no `fi`

DragonFly 6.4.2's fusefs does **not** pass `fi` to `truncate` at all
(`TRUNCATE /f size=100 fi=0`), even though FreeBSD 15.1 — its ancestor — does
(`fi=1 fh=1`).  Without `fi` there is no `fi->fh` to reach the inode by, so
`ftruncate(2)` on an unlinked-but-still-open descriptor returns `ENOENT`.

## NetBSD (librefuse): `fi` on linked, dropped on unlink

NetBSD 11.0's librefuse (PUFFS) **does** pass `fi->fh` to `truncate` on a
linked fd (`TRUNCATE /f size=100 fi=1 fh=1`) — unlike DragonFly — but drops the
handle once the name is unlinked, so `ftruncate` after unlink falls through to
the path and returns `ENOENT`.  This is why `configure` treats librefuse
separately from real fuse3 when it decides whether `test_fuse_unlink_open`
should skip (`FILSYS_TRUNC_FI` is set for real fuse3 only).

## CI results

All six roadmap BSDs are green.  The three that initially failed in `prepare`
were VM-image/package-manager issues, fixed as follows:

- **NetBSD** — the on-boot package-mirror probe timed out ("no pkg found for
  curl"); fixed by using the canonical `/usr/sbin/pkg_add -u`.  The conformance
  probe links `-lrefuse -lpuffs` (no `fuse3.pc`).
- **MidnightBSD** — its package manager is `mport`, not `pkg`; fixed to
  `mport install`.
- **HardenedBSD** — `hardening.harden_rtld=1` refused to run the freshly
  installed binaries (`Tainted process refusing to run binary`); fixed by
  `sysctl hardening.harden_rtld=0` first.
