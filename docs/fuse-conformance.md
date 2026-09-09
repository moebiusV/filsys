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
| OpenBSD (fuse2) | — | — | — | — | — | — |
| NetBSD (fuse3) | — | — | — | — | — | — |
| FreeBSD (fuse3) | — | — | — | — | — | — |
| HardenedBSD | — | — | — | — | — | — |
| DragonFly BSD | — | — | — | — | — | — |
| MidnightBSD | — | — | — | — | — | — |
| illumos (libfuse 2.7.6) | — | — | — | — | — | — |

`—` means not yet measured.  Fill a row by running `sh conformance/run.sh` on
that platform and recording the six answers here.
