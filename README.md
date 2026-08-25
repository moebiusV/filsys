# v7fuse

Mount a **Seventh Edition (V7) Unix** filesystem image (PDP-11) as a FUSE
filesystem on Linux, so files can be copied on and off the disk for use with a
simulator (SIMH `pdp11`).

The target image is the RP06 root filesystem from
[`pcollinson/unixv7-extras`](https://github.com/pcollinson/unixv7-extras/tree/main/bootstrap).

## Build

No root needed — the libfuse3 dev headers are downloaded and extracted locally.

```sh
./fetch.sh     # fetch disk image + libfuse3 headers (once)
make           # builds ./v7mount
```

`make` uses `gcc -std=c17` (C17, not C23, to match Microsoft's toolchain) and
links against the system `libfuse3.so.4` runtime directly, since only the dev
package provides the `libfuse3.so` symlink.

## Usage

```sh
./v7mount [options] <image> <mountpoint>
```

| option | meaning                          |
|--------|----------------------------------|
| `-r`   | mount read-only                  |
| `-f`   | stay in foreground               |
| `-d`   | FUSE debug output                |

```sh
mkdir mnt
./v7mount rp06-0.disk mnt        # read-write (make a copy first!)
ls mnt
cp mnt/etc/passwd .               # copy a file off
cp host.txt mnt/tmp/              # copy a file on
fusermount3 -u mnt                # unmount
```

## Filesystem format (verified against the image and the V7 source)

Block size 512; block 0 = boot, block 1 = superblock, blocks 2..`s_isize`-1 =
i-list (8 inodes/block), data starts at `s_isize`.  The PDP-11 is
**middle-endian**:

- 16-bit fields: little-endian.
- 32-bit fields (`daddr_t`/`off_t`/`time_t`): high word first, each word
  little-endian.
- 3-byte block pointers in `di_addr`: the low three bytes of that middle-endian
  layout, i.e. `[ hi, lo, mid ]`.
- indirect blocks: 4-byte middle-endian `daddr_t`, 128 entries/block.

`v7fs.c` implements `balloc`/`bfree`/`ialloc`/`ifree`/`itrunc` mirroring the V7
kernel's `sys/alloc.c`, so the free list stays interchangeable with what a
running V7 kernel expects.  See `reference/` for the V7 source excerpts and the
RP06 partition table.

## Coordination with the simulator

The image must not be written by both the emulator and FUSE at once.  `v7mount`
holds an **exclusive `flock`** on the image for the whole mount; the patched
SIMH takes that same lock around each sector read/write.  So the emulator
blocks — pauses — while the image is mounted, and resumes on unmount.

> **Rule: the simulator must not run while the disk is mounted.**  The lock
> enforces this (a running emulator's next disk I/O blocks until unmount), but
> the deeper reason is V7's in-memory state: the running kernel caches the
> superblock free list, the inode table, and the buffer cache, so editing the
> disk behind it leaves those stale.  The safe workflow is to stage files while
> V7 is *not* running, then boot it fresh.  (`sync` inside V7 before halting
> flushes its buffers; the lock only prevents concurrent *writes*, it cannot
> refresh a running kernel's caches.)

To patch and rebuild the simulator:

```sh
cd ../simh && make pdp11    # see sim_disk.c (SIM_DISK_LOCK/SIM_DISK_UNLOCK)
```

## Layout

- `v7fs.h` / `v7fs.c` — filesystem access layer (byte-order, superblock,
  inodes, block mapping, allocation, directories).
- `v7mount.c` — FUSE (libfuse3) high-level callbacks and the mount CLI.
- `Makefile`, `fetch.sh`, `test.sh`, `reference/`.

## Notes

- Mount read-write only on a **copy** of the image; the driver has been
  smoke-tested but V7 has no journal — a bug corrupts the image.
- `simh` (SIMH V4.0-0) has a pre-existing, non-deterministic `double free` in
  its quit-after-attach cleanup path; it is unrelated to the flock patch.
