# v7fuse

Mount a **Seventh Edition (V7) Unix** filesystem image (PDP-11) as a FUSE
filesystem on Linux, so files can be copied on and off the disk for use with a
simulator (SIMH `pdp11`).

v7fuse understands the V7 on-disk format exactly — middle-endian byte order and
the kernel's own free-list allocation discipline — so files you stage with it
are seen by a running V7 kernel after you boot the image.

## Dependencies

- a C17 compiler (`gcc` or `clang`)
- **libfuse3** (`libfuse3-dev` on Debian/Ubuntu, `fuse3-devel` on Fedora, `fuse3` on Arch)

If you can't install the dev package, `./fetch.sh` downloads and extracts the
libfuse3 headers locally and the build falls back to linking the runtime
SONAME directly.

## Build

```sh
autoreconf -i        # first time only (generates ./configure)
./configure
make
sudo make install    # installs v7mount + v7mount(1)
```

`./configure && make && make install` is the standard GNU flow.  The code is
compiled as C17 (not C23) to match Microsoft's toolchain ceiling.

## Usage

```sh
v7mount [options] <image> <mountpoint>
v7mount -c <image>           # integrity check (no mount)
```

| option | meaning                          |
|--------|----------------------------------|
| `-r`   | mount read-only                  |
| `-f`   | stay in foreground               |
| `-d`   | FUSE debug output                |
| `-c`   | free-list / inode-table check    |

```sh
mkdir mnt
v7mount rp06-0.disk mnt        # read-write (make a copy first!)
ls mnt
cp mnt/etc/passwd .             # copy a file off
cp host.txt mnt/tmp/            # copy a file on
fusermount3 -u mnt              # unmount

v7mount -c rp06-0.disk          # verify the free list + inode table
```

See `v7mount(1)` for details.

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
holds an **exclusive `flock`** on the image for the whole mount; a simulator
patched to take that same lock around its own disk I/O will block (pause)
while the image is mounted, and resume on unmount.

> **Rule: the simulator must not run while the disk is mounted.**  The lock
> marks the image busy, but the deeper reason is V7's in-memory state: the
> running kernel caches the superblock free list, the inode table, and the
> buffer cache, so editing the disk behind it leaves those stale.  The safe
> workflow is to stage files while V7 is *not* running, then boot it fresh.
> (`sync` inside V7 before halting flushes its buffers; the lock only marks
> the image busy, it cannot refresh a running kernel's caches.)

## Booting the image in SIMH

The `runv7/` directory has a headless boot setup: `headless.ini` configures a
PDP-11/70 with the RP06 image and exposes the console over telnet, and
`headless_boot.py` connects and drives the two-stage bootstrap.  The boot
sequence is `boot rp0` → type `boot` → at `:` type `hp(0,0)unix`.

## Layout

- `v7fs.h` / `v7fs.c` — filesystem access layer (byte-order, superblock,
  inodes, block mapping, allocation, directories).
- `v7mount.c` — FUSE (libfuse3) high-level callbacks and the mount CLI.
- `configure.ac`, `Makefile.am` — GNU autotools build.
- `test.sh`, `fetch.sh`, `reference/`, `runv7/`.

## Notes

- Mount read-write only on a **copy** of the image; the driver has been
  smoke-tested but V7 has no journal — a bug corrupts the image.
