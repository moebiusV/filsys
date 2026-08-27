# kenfs

Mount a **Research Unix** filesystem image (PDP-11) as a FUSE filesystem on
Linux, so files can be copied on and off the disk for use with a simulator
(SIMH `pdp11`).

One binary, every edition we care about — the on-disk format is understood
exactly (middle-endian byte order and the kernel's own free-list allocation
discipline), so files staged with it are seen by a running kernel after you
boot the image.

```
kenfsmount -v 6 v6root.dsk mnt        # V6 format (also V4 and V5 — identical on disk)
kenfsmount -v 7 rp06-0.disk mnt       # V7 format
kenfsmount -v 32 32vroot.dsk mnt      # 32V format (V7 for the VAX, little-endian)
```

Home: <https://github.com/moebiusV/kenfs>

## Dependencies

- a C17 compiler (`gcc` or `clang`)
- **libfuse3** (`libfuse3-dev` on Debian/Ubuntu, `fuse3-devel` on Fedora, `fuse3` on Arch)

If you can't install the dev package, `./fetch.sh` downloads and extracts the
libfuse3 headers locally and the build falls back to linking the runtime
SONAME directly.

## Build

```sh
./configure
make
sudo make install    # installs kenfsmount + kenfs.5
```

`./configure && make && make install` is the standard GNU flow; `configure` is
shipped so no autotools are needed.  (To build from a git checkout after
editing `configure.ac` or `Makefile.am`, run `autoreconf -i` first.)  The code
is compiled as C17 (not C23) to match Microsoft's toolchain ceiling.

## Usage

```sh
kenfsmount -v <4|5|6|7|32> [options] <image> <mountpoint>
kenfsmount -v <4|5|6|7|32> -c <image>        # integrity check (no mount)
```

`-v` takes the Unix edition — `4`, `5`, `6`, `7` or `32` (a leading `v`, as in
`v7`, is accepted; so is `32v`).  `4` and `5` are byte-identical to `6`, so
they share one code path.

| option | meaning                          |
|--------|----------------------------------|
| `-v 4` | V4 format (byte-identical to V5/V6) |
| `-v 5` | V5 format (byte-identical to V4/V6) |
| `-v 6` | V6 format                          |
| `-v 7` | V7 format                          |
| `-v 32`| 32V format (little-endian V7)      |
| `-r`   | mount read-only                  |
| `-f`   | stay in foreground               |
| `-d`   | FUSE debug output                |
| `-c`   | free-list / inode-table check    |

```sh
mkdir mnt
kenfsmount -v 7 rp06-0.disk mnt        # read-write (make a copy first!)
ls mnt
cp mnt/etc/passwd .             # copy a file off
cp host.txt mnt/tmp/            # copy a file on
fusermount3 -u mnt              # unmount

kenfsmount -v 6 -c v6root.dsk          # verify the free list + inode table
```

See `kenfs.5` for both the tool and the on-disk format.

## Implementor's Notes

These are the format facts learned the hard way while building this, folded
together with the history of the filesystem itself.  They are the
documentation of record for the `v6fs.c` / `v7fs.c` backends.

### History

- The filesystem was designed on blackboards and scribbled notes in 1969 by
  **Ken Thompson, Dennis Ritchie and Rudd Canaday**.  Thompson was the
  architect; Ritchie claims the one idea of *device files*; Canaday is the
  third name almost nobody remembers.  The design **predates the hardware** —
  Thompson modeled its disk behaviour on Multics (GE-645) before there was a
  computer to run it on — and the filesystem **predates the operating
  system**: it was built on the PDP-7 first, and the exercising programs
  (editor, assembler, kernel) grew into Unix almost by accident in the summer
  of 1969.
- **V1–V3** (1971–73) kernels are PDP-11 **assembly**.  Directory entries were
  **10 bytes** (2-byte i-number + 8-char name).  The i-list, directories-as-
  files, and device files were already there from the 1969 design.
- **V4 (1973)** is the famous **C rewrite** of the kernel *and* the
  filesystem, and the first edition with the **16-byte directory entry**
  (`d_ino` + `d_name[14]`) — the same struct that survives into V7's
  `dir.h`.  The 14 is an artifact of making the entry 16 bytes once the
  i-number took two.
- **V5, V6 (1974–75)**: **no on-disk change** from V4.  The V6 inode is 32
  bytes, eight 16-bit block addresses with the `ILARG` flag switching them to
  indirect, and a 24-bit size split across a byte and a word.
- **V7 (1979)** is the **only format break** in the range.  Disks had grown
  enough that 16-bit block numbers were a handicap, so V7 widened block
  numbers to 24 bits (packed three to a byte-triple by `l3tol`/`ltol3`),
  doubled the inode to 64 bytes (13 addresses: ten direct + single/double/
  triple indirect), made the size a full 32 bits, and added `ctime`.  The
  widening is generally Ken Thompson's, driven by the same portability work
  (Johnson and Ritchie's Interdata port) that produced `daddr_t`.
- **32V** is V7 recompiled for the VAX — structurally identical, but see the
  byte-order caveat below.

### Format table

| | V4 / V5 / V6 (identical) | V7 | 32V |
|---|---|---|---|
| block size | 512 | 512 | 512 |
| inode size | 32 B (16/block) | 64 B (8/block) | 64 B (8/block) |
| block addresses | 8 × 16-bit | 13 × 24-bit (3-byte packed) | same |
| file size | 24-bit | 32-bit | 32-bit |
| superblock free list | `free[100]`, 16-bit | `s_free[50]`, 24-bit | same |
| root inode | 1 | 2 | 2 |
| bad-block file | none | inode 1 | inode 1 |
| directory entry | 16 B (`d_ino` + 14-char) | 16 B | 16 B |

One `-v 6` code path covers V4, V5 and V6 — they are byte-identical on disk.

### Gotchas

- **The V6 24-bit size is `(size0 << 16) | size1`.**  `size0` (one byte at
  inode offset +5) is the *high* byte and `size1` (the word at +6) is the
  *low* 16 bits — not the other way around.  Getting this backwards makes a
  160-byte root directory read as 40960 and every write fail `ENOSPC`.
- **Root inode differs by edition.**  V4/V5/V6 have `ROOTINO 1` and no
  bad-block file; V7/32V have `ROOTINO 2` and reserve inode 1.  Do not carry
  the V7 "root is 2 / inode 1 is bad blocks" convention back to V6.
- **The V7 bad-block file.**  Inode 1 is a regular file with `nlink = 0`
  (nameless), and its `di_addr` entries *are* the bad block numbers,
  marking them allocated.  The kernel's `badblock()` in `alloc.c` is
  unrelated — it is just a range check; the bad-block *list* is used by
  `mkfs`/`fsck`/`icheck`, not by the running allocator.  Stock V7 `mkfs`
  stubs `badblk()` out and creates an empty bad-block file.
- **A device file's `i_addr[0]` is not a block number — it is the device
  number.**  In V4–V7 a character or block special file stores its
  major/minor device number in `i_addr[0]` (e.g. `/dev/tty0` is `0x300`,
  `/dev/null` is `0x800`, `/dev/mem` is `0x100`) and leaves the rest of
  `i_addr` zero.  Those small numbers collide with the low data-block numbers,
  so a naive "walk every inode and flag a block owned twice" checker reports a
  **double allocation** where there is none — `/bin/cdb` owns block 768, and
  `/dev/tty0` is *also* "768", but only as a device number.  For a while this
  looked like a genuine "two files sharing a block" corruption on the V6
  image; it was always the device files.  A block-ownership audit must skip
  type `IFCHR`/`IFBLK` before reading `i_addr` as block pointers.
- **V6 added `int pad[50]` to the superblock struct; V4/V5 have no pad.**  The
  V6 `filsys` is 516 bytes — 4 bytes *over* the 512-byte block.  It does
  **not** spill into block 2: V6's `bcopy` counts in 16-bit *words* (its body
  is `*b++ = *a++` over `int *`), and the superblock I/O calls `bcopy(..., 256)`
  = 256 words = 512 bytes, so the last 4 bytes of `pad[50]` (`pad[48]`,
  `pad[49]`) are never written to disk.  Block 2 is the start of the i-list
  and is untouched.  The pad looks like an over-count — `pad[48]` (96 bytes)
  would have landed the struct on exactly 512; `pad[50]` overshoots by 4.
- **32V is not byte-identical to V7 at all.**  *Every* 32-bit field flips byte
  order: `di_size`, `di_atime`/`di_mtime`/`di_ctime`, `s_fsize`, `s_free[]`,
  `s_time`, and the indirect-block `daddr_t` entries.  And — correcting a
  claim made earlier in this project — the **3-byte `di_addr` addresses differ
  too**: the V7 `iexpand` packs them `[hi, lo, mid]`, the 32V `iexpand` packs
  them `[lo, mid, hi]`.  The two kernels' `iexpand` differ in exactly where the
  zero pad byte goes (byte 1 on the PDP-11, byte 3 on the VAX).  Only the
  16-bit fields (`di_mode`, `di_nlink`, `di_uid`, `di_gid`, `s_isize`,
  `s_nfree`, `s_ninode`, `s_inode[]`) are byte-order neutral.  kenfs handles
  this with a `-32` selector.
- **V3-and-earlier directories are 10 bytes**, so a reader for those editions
  needs a different directory walker.  Out of scope here (we floor at V4).

### Byte order (PDP-11)

The PDP-11 is **middle-endian**:

- 16-bit fields: little-endian.
- 32-bit fields (`daddr_t`/`off_t`/`time_t`): high word first, each word
  little-endian.
- 3-byte block pointers in `di_addr` (V7): the low three bytes of that
  middle-endian layout, i.e. `[ hi, lo, mid ]`.
- V7 indirect blocks: 4-byte middle-endian `daddr_t`, 128 entries/block.
- V6 indirect blocks: 2-byte little-endian block numbers, 256 entries/block.
- **32V (VAX)** is little-endian throughout: 32-bit fields low word first, and
  the 3-byte `di_addr` packed `[lo, mid, hi]`.  The `-32` selector flips all of
  these in `v7fs.c` (the `le` byte-order flag).

`v6fs.c` and `v7fs.c` implement `balloc`/`bfree`/`ialloc`/`ifree`/`itrunc`
mirroring the respective kernel's `sys/alloc.c`, so the free list stays
interchangeable with what a running kernel expects.

## Verification

The read path and the write path were both exercised against real images of
every edition:

| edition | image | `-c` | mount | create/write | chmod/chown | delete |
|---|---|---|---|---|---|---|
| V4 | TUHS `Utah_v4/disk.rk` | ✅ | ✅ | ✅ | ✅ (on-disk bytes verified) | ✅ |
| V5 | TUHS `Dennis_v5/v5root` | ✅ | ✅ | ✅ | ✅ | ✅ |
| V6 | pcollinson `rk0` / SIMH `uv6swre` | ✅ | ✅ | ✅ | ✅ | ✅ |
| V7 | pcollinson `rp06-0.disk` | ✅ | ✅ | ✅ | ✅ | ✅ |

On-disk verification dumped the raw 32-byte inode blocks after `chmod`/`chown`
and confirmed the mode, uid/gid and size fields landed correctly, and that
delete freed the inode and data block (free counts restored, `errors=0`).

## Coordination with the simulator

The image must not be written by both the emulator and FUSE at once.  `kenfs`
holds an **exclusive `flock`** on the image for the whole mount; a simulator
patched to take that same lock around its own disk I/O will block (pause)
while the image is mounted, and resume on unmount.

> **Rule: the simulator must not run while the disk is mounted.**  The lock
> marks the image busy, but the deeper reason is the kernel's in-memory state:
> the running kernel caches the superblock free list, the inode table, and the
> buffer cache, so editing the disk behind it leaves those stale.  The safe
> workflow is to stage files while the system is *not* running, then boot it
> fresh.  (`sync` inside before halting flushes its buffers; the lock only
> marks the image busy, it cannot refresh a running kernel's caches.)

## Layout

- `v6fs.h` / `v6fs.c` — V4/V5/V6 on-disk access layer.
- `v7fs.h` / `v7fs.c` — V7 on-disk access layer.
- `kenfsmount.c` — FUSE callbacks + the `-6`/`-7`/`-32` edition selector.
- `kenfs.5` — the folded filesystem-format manpage (V4 through 32V).
- `configure.ac`, `Makefile.am` — GNU autotools build.
- `test.sh`, `fetch.sh`, `reference/`, `runv7/`.

## Notes

- Mount read-write only on a **copy** of the image; V4–V7 have no journal —
  a bug corrupts the image.
- The `-c` integrity check walks the free list and the inode table and
  reports out-of-range block numbers, cycles, and unreadable inodes.
- The on-disk `fsize` and every inode's `size` are validated against the real
  image size and the data area **before any allocation**, so a corrupt image
  cannot trigger a multi-gigabyte `malloc` or an unbounded loop.  (libFuzzer +
  ASan/UBSan found this class of bug before it shipped; the read path fuzzes
  clean, and `gcc -fanalyzer` / `clang --analyze` are quiet.)

## License

The original code (`v6fs.c`, `v7fs.c`, `kenfsmount.c`, and their headers) is
licensed under the **ISC license** — Copyright (c) 2026 David Walther.

The `kenfs.5` manpage is derived from the ancient UNIX `fs(5)` (V4, V6) and
`filsys(5)`/`dir(5)` (V7, 32V) pages, and retains the **Caldera International
"Ancient UNIX License"** (2002) notice and terms, as that license requires for
redistribution of derived documentation.

Both texts are in [`COPYING`](COPYING).
