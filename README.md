# filsys

Mount a **Research Unix** filesystem image (PDP-11) as a FUSE filesystem on
Linux, so files can be copied on and off the disk for use with a simulator
(SIMH `pdp11`).

One binary, every edition we care about: the on-disk format is understood
(middle-endian byte order and the kernel's own free-list allocation
discipline), so files staged with it are seen by a running kernel after you
boot the image.

```
filsysmount -v 6 v6root.dsk mnt        # V6 format (also V4 and V5, identical on disk)
filsysmount -v 7 rp06-0.disk mnt       # V7 format
filsysmount -v 32 32vroot.dsk mnt      # 32V format (V7 for the VAX, little-endian)
```

Home: <https://github.com/moebiusV/filsys>

## Dependencies

- a C17 compiler (`gcc` or `clang`)
- **libfuse3** (`libfuse3-dev` on Debian/Ubuntu, `fuse3-devel` on Fedora, `fuse3` on Arch)

`mkfs` and `fsck` are not built here: they are ported in v7unix-toolchain
(<https://github.com/moebiusV/v7unix-toolchain>), and filsys recommends
it to provide them.

If you can't install the dev package, `./fetch.sh` downloads and extracts the
libfuse3 headers locally and the build falls back to linking the runtime
SONAME directly.

## Build

```sh
./configure
make
sudo make install    # installs filsysmount, filsysfind + their manpages
```

`./configure && make && make install` is the standard GNU flow; `configure` is
shipped so no autotools are needed.  (To build from a git checkout after
editing `configure.ac` or `Makefile.am`, run `autoreconf -i` first.)  The code
is compiled as C17 (not C23) to match Microsoft's toolchain ceiling.

## Usage

```sh
filsysmount -v <4|5|6|7|32> [options] <image> <mountpoint>
filsysmount -v <4|5|6|7|32> -c <image>        # integrity check (no mount)
```

`-v` takes the Unix edition: `4`, `5`, `6`, `7` or `32` (a leading `v`, as in
`v7`, is accepted; so is `32v`).  `4` and `5` are byte-identical to `6`, so
they share one code path.

| option | meaning                          |
|--------|----------------------------------|
| `-v 4` | V4 format (byte-identical to V5/V6) |
| `-v 5` | V5 format (byte-identical to V4/V6) |
| `-v 6` | V6 format                          |
| `-v 7` | V7 format                          |
| `-v 32`| 32V format (little-endian V7)      |
| `-o offset=N` | mount a filesystem at byte offset N (a partition) |
| `-o uid=N,gid=N` | override reported ownership (default: you) |
| `-o allow_other,...` | pass a FUSE option through |
| `-r`   | mount read-only                  |
| `-f`   | stay in foreground               |
| `-d`   | FUSE debug output                |
| `-c`   | free-list / inode-table check    |

```sh
mkdir mnt
filsysmount -v 7 rp06-0.disk mnt        # read-write (make a copy first!)
ls mnt
cp mnt/etc/passwd .             # copy a file off
cp host.txt mnt/tmp/            # copy a file on
fusermount3 -u mnt              # unmount

filsysmount -v 6 -c v6root.dsk          # verify the free list + inode table
```

See `filsys.5` for both the tool and the on-disk format.

## Implementor's Notes

These are the format facts learned the hard way while building this, folded
together with the history of the filesystem itself.  They are the
documentation of record for the `v6fs.c` / `v7fs.c` backends.

### History

- The filesystem was designed on blackboards and scribbled notes in 1969 by
  **Kenneth Lane Thompson, Dennis MacAlistair Ritchie and Rudd Canaday**.
  Thompson was the
  architect; Ritchie claims the one idea of *device files*; Canaday is the
  third name.  The design **predates the hardware**; Thompson modeled its disk
  behaviour on Multics (GE-645) before there was a computer to run it on, and
  the filesystem **predates the operating system**: it was built on the PDP-7
  first, and the exercising programs (editor, assembler, kernel) grew into
  Unix in the summer of 1969.
- The name **filsys** is an homage to the original filesystem code: the on-disk
  superblock has been `struct filsys` (short for "file system") in the Unix
  headers from day one, entering the source as `filsys.h`.  The term itself
  came from the GE GECOS mainframe the early Bell Labs PDP-7/11 sat alongside,
  which is why `/etc/passwd` still carries a `GECOS` field half a century later.
- **V1-V3** (1971-73) kernels are PDP-11 **assembly**.  Directory entries were
  **10 bytes** (2-byte i-number + 8-char name).  The i-list, directories-as-
  files, and device files were already there from the 1969 design.
- **V4 (1973)** is the **C rewrite** of the kernel *and* the filesystem, and
  the first edition with the **16-byte directory entry** (`d_ino` +
  `d_name[14]`), the same struct that survives into V7's `dir.h`.  The 14 is
  an artifact of making the entry 16 bytes once the i-number took two.  The V4
  inode is **32 bytes** (16 per block): eight 16-bit block addresses, the
  `ILARG` flag switching them to indirect, and a 24-bit size split across a
  byte and a word.
- **V5, V6 (1974-75)**: **no on-disk change** from V4.  V6 alone added
  `int pad[50]` to the superblock struct, but the extra bytes never reach disk
  (see the pad note below), so the on-disk layout is unchanged.
- **V7 (1979)** is the **only format break** in the range.  Disks had grown
  enough that 16-bit block numbers were a handicap, so V7 widened block
  numbers to 24 bits (packed three to a byte-triple by `l3tol`/`ltol3`),
  doubled the inode to 64 bytes (13 addresses: ten direct + single/double/
  triple indirect), made the size a full 32 bits, and added `ctime`.  The
  widening is generally Ken Thompson's, driven by the same portability work
  (Johnson and Ritchie's Interdata port) that produced `daddr_t`.
- **32V** is V7 recompiled for the VAX; structurally identical, but many
  fields have a different byte order (see below).

### Format table

| | V4 / V5 / V6 (identical) | V7 | 32V |
|---|---|---|---|
| block size | 512 | 512 | 512 |
| inode size | 32 B (16/block) | 64 B (8/block) | 64 B (8/block) |
| block addresses | 8 x 16-bit | 13 x 24-bit (3-byte packed) | same |
| file size | 24-bit | 32-bit | 32-bit |
| superblock free list | `free[100]`, 16-bit | `s_free[50]`, 24-bit | same |
| root inode | 1 | 2 | 2 |
| bad-block file | none | inode 1 | inode 1 |
| directory entry | 16 B (`d_ino` + 14-char) | 16 B | 16 B |

One `-v 6` code path covers V4, V5 and V6, which are byte-identical on disk.

### Gotchas

- **The V6 24-bit size is `(size0 << 16) | size1`.**  `size0` (one byte at
  inode offset +5) is the *high* byte and `size1` (the word at +6) is the
  *low* 16 bits, not the other way around.  Getting this backwards makes a
  160-byte root directory read as 40960 and every write fail `ENOSPC`.
- **Root inode differs by edition.**  V4/V5/V6 have `ROOTINO 1` and no
  bad-block file; V7/32V have `ROOTINO 2` and reserve inode 1.  Do not carry
  the V7 "root is 2 / inode 1 is bad blocks" convention back to V6.
- **The V7 bad-block file.**  Inode 1 is a regular file with `nlink = 0`
  (nameless), and its `di_addr` entries *are* the bad block numbers,
  marking them allocated.  The kernel's `badblock()` in `alloc.c` is
  unrelated; it is just a range check, and the bad-block *list* is used by
  `mkfs`/`fsck`/`icheck`, not by the running allocator.  Stock V7 `mkfs`
  stubs `badblk()` out and creates an empty bad-block file.
- **A device file's `i_addr[0]` is not a block number; it is the device
  number.**  In V4-V7 a character or block special file stores its
  major/minor device number in `i_addr[0]` (e.g. `/dev/tty0` is `0x300`,
  `/dev/null` is `0x800`, `/dev/mem` is `0x100`) and leaves the rest of
  `i_addr` zero.  Those small numbers collide with the low data-block numbers,
  so a naive "walk every inode and flag a block owned twice" checker reports a
  **double allocation** where there is none: `/bin/cdb` owns block 768, and
  `/dev/tty0` is *also* "768", but only as a device number.  For a while this
  looked like a genuine "two files sharing a block" corruption on the V6
  image; it was always the device files.  A block-ownership audit must skip
  type `IFCHR`/`IFBLK` before reading `i_addr` as block pointers.
- **V6 added `int pad[50]` to the superblock struct; V4/V5 have no pad.**  The
  V6 `filsys` is 516 bytes, 4 bytes *over* the 512-byte block.  It does
  **not** spill into block 2: V6's `bcopy` counts in 16-bit *words* (its body
  is `*b++ = *a++` over `int *`), and the superblock I/O calls `bcopy(..., 256)`
  = 256 words = 512 bytes, so the last 4 bytes of `pad[50]` (`pad[48]`,
  `pad[49]`) are never written to disk.  Block 2 is the start of the i-list
  and is untouched.  The pad looks like an over-count: `pad[48]` (96 bytes)
  would have landed the struct on exactly 512; `pad[50]` overshoots by 4.
- **32V is not byte-identical to V7 at all.**  *Every* 32-bit field flips byte
  order: `di_size`, `di_atime`/`di_mtime`/`di_ctime`, `s_fsize`, `s_free[]`,
  `s_time`, and the indirect-block `daddr_t` entries.  The **3-byte `di_addr`
  addresses differ too**: the V7 `iexpand` packs them `[hi, lo, mid]`, the 32V
  `iexpand` packs them `[lo, mid, hi]`.  The two kernels' `iexpand` differ in
  exactly where the zero pad byte goes (byte 1 on the PDP-11, byte 3 on the
  VAX).  Only the 16-bit fields (`di_mode`, `di_nlink`, `di_uid`, `di_gid`,
  `s_isize`, `s_nfree`, `s_ninode`, `s_inode[]`) are byte-order neutral.
  filsys handles this with a `-v 32` selector.
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
  the 3-byte `di_addr` packed `[lo, mid, hi]`.  The `-v 32` selector flips all
  of these in `v7fs.c` (the `le` byte-order flag).
- **32V also shifts several *superblock* fields two bytes later**: the VAX
  aligns `daddr_t`/`time_t` to 4 bytes, so `s_fsize` moves +2->+4, `s_nfree`
  +6->+8, `s_free[]` +8->+12, `s_ninode` +208->+212, and `s_time` +414->+420
  (each 2-byte field `s_isize`/`s_nfree`/`s_ninode` gains a 2-byte pad after
  it).  The free-list dump block shifts the same way: `df_nfree` is a 4-byte
  `int` on the VAX (2 bytes on the PDP-11), so `df_free[]` moves +2->+4.  The
  inode and directory entry are *not* shifted (their fields already fall on
  4-byte boundaries), so only `filsys` and `fblk` differ in *layout*; every
  other structure differs from V7 only in byte order.

`v6fs.c` and `v7fs.c` implement `balloc`/`bfree`/`ialloc`/`ifree`/`itrunc`
mirroring the respective kernel's `sys/alloc.c`, so the free list stays
interchangeable with what a running kernel expects.

### Disk partitions

A V7 disk is a **partitioned** disk, and the partition table is *not on the
disk*; it is compiled into the kernel's device driver (`rp.c`'s `rp_sizes`,
`rk.c`'s table).  The pcollinson RP06 images divide their 340,671 blocks as:

| partition | blocks | size | holds |
|---|---|---|---|
| root (`/dev/rp0`) | 0-4999 | 2.5 MB | `/`, `etc/rc`, `/unix` |
| swap + spare | 5000-18391 | 6.5 MB | (mostly zeroed) |
| `/usr` (`/dev/rp3` = `rp0h`) | 18392-340669 | 165 MB | full source tree |

The root's `/etc/rc` gives it away: `mount /dev/rp3 /usr`, and `/dev/rp3` is a
block device (major 6, minor 7 = `rp0h`).  So the "root smaller than the disk"
is not waste; it is the normal V7 root/swap//usr split, and the `/usr`
filesystem sits **intact** at block 18392 (superblock at 18393: `isize=8189`,
`fsize=322278`, middle-endian).  Mount it in place with the byte offset
(`18392 x 512 = 9416704`): `filsysmount -v 7 -o offset=9416704 rp06-0.disk mnt`,
and `-c` reports 2064 used inodes, `errors=0`; it mounts as a complete
May-1979 source tree (`/usr/src`, `/usr/sys`, man pages, games).

To locate such a partition you read `/etc/rc` (for the *name*), read the
driver's partition table (for the *offset*), or run **`filsysfind`** (see
below), which scans for superblocks at cylinder boundaries and, with `-i`,
traces inode-table runs backwards to their superblocks.  Do not cap `isize`
too low: this `/usr` has `isize=8189` (65,512 inodes), which a naive
"small i-list" heuristic wrongly skips.

`filsysmount -o offset=N` shifts the superblock read to byte `N`, so a
partition mounts in place without `dd`, and the root and `/usr` partitions
can be mounted from the *same* file at once, nested:

```
filsysmount -v 7 rp06-0.disk mnt/
filsysmount -v 7 -o offset=9416704 rp06-0.disk mnt/usr
```

The second (nested) mount works because filsysmount reports files as the
mounting user (override with `-o uid=,gid=`), so the inner mount point is
owned by you.

### Mount commands

Copy-paste commands per image (images distributed by the
[prebsd](https://github.com/moebiusV/prebsd) project):

    # V7 (rp06-0.disk): root 0-4999, swap 5000-18391, /usr 18392+
    filsysmount -v 7  rp06-0.disk mnt
    filsysmount -v 7  -o offset=9416704 rp06-0.disk mnt/usr

    # 32V (32v-rp06.disk): same layout as V7
    filsysmount -v 32 32v-rp06.disk mnt
    filsysmount -v 32 -o offset=9416704 32v-rp06.disk mnt/usr

    # single-filesystem images
    filsysmount -v 32 32v-root.disk mnt       # 32V root only
    filsysmount -v 6  rk0 mnt                 # V6 root only

Mount the root first, then nest the `/usr` mount on top.

### Finding partitions (filsysfind)

`filsysfind` locates the filesystems on a raw image.  It scans for superblocks
(validating the edition, i-list and volume sizes, and that the free list holds
only in-range blocks), and with `-i` also scans for inode-table runs and
traces backwards to their superblocks.  Scan at cylinder boundaries to dodge
the false positives a block-by-block sweep of file data produces:

```
filsysfind -c 418 rp06-0.disk
# fs @ block 0      (byte 0)       V7  isize=202  fsize=5000
# fs @ block 18392  (byte 9416704) V7  isize=8189 fsize=322278
```

Mount any hit with `filsysmount -o offset=<byte>`.

## Verification

The read path and the write path were both exercised against real images of
every edition:

| edition | image | `-c` | mount | create/write | chmod/chown | delete |
|---|---|---|---|---|---|---|
| V4 | TUHS `Utah_v4/disk.rk` | yes | yes | yes | yes (on-disk bytes verified) | yes |
| V5 | TUHS `Dennis_v5/v5root` | yes | yes | yes | yes | yes |
| V6 | pcollinson `rk0` / SIMH `uv6swre` | yes | yes | yes | yes | yes |
| V7 | pcollinson `rp06-0.disk` | yes | yes | yes | yes | yes |
| 32V (VAX) | `32v-root.disk`, `32v-rp06.disk` (`/usr`) | yes | yes | yes | yes | yes |

On-disk verification dumped the raw 32-byte inode blocks after `chmod`/`chown`
and confirmed the mode, uid/gid and size fields landed correctly, and that
delete freed the inode and data block (free counts restored, `errors=0`).

32V has no published disk image, so the test image was built from scratch:
compile open-simh's VAX-11/780 (`vax780`, which requires the `vmb.exe` ROM),
boot 32V, and install it from a tape image.

## Coordination with the simulator

> **Rule: the simulator must not run while the disk is mounted.**  The running
> kernel caches the superblock free list, the inode table, and the buffer
> cache, so editing the disk behind it leaves those stale.  The safe workflow
> is to stage files while the system is *not* running, then boot it fresh.
> (`sync` inside before halting flushes its buffers.)

filsysmount deliberately takes **no lock** on the image: a V7 disk is a set of
partitions in one file, and mounting the root and `/usr` at two mount points
from the same file at once requires both mounts to share it read-only.  The
"don't edit a disk under a running kernel" rule above is the real protection;
the file is never locked, so it is on you not to mount read-write while the
emulator is running.

## Layout

- `v6fs.h` / `v6fs.c`: V4/V5/V6 on-disk access layer.
- `v7fs.h` / `v7fs.c`: V7/32V on-disk access layer.
- `filsysmount.c`: FUSE callbacks + the `-v` edition selector.
- `filsysfind.c`: locate filesystem superblocks (partitions) on a raw image.
- `filsys.5`, `filsysfind.1`: the format and tool manpages.
- `configure.ac`, `Makefile.am`: GNU autotools build.
- `test.sh`, `fetch.sh`, `reference/`.

## Notes

- Mount read-write only on a **copy** of the image; V4-V7 have no journal; a
  bug corrupts the image.
- The `-c` integrity check walks the free list and the inode table and
  reports out-of-range block numbers, cycles, and unreadable inodes.
- The on-disk `fsize` and every inode's `size` are validated against the real
  image size and the data area **before any allocation**, so a corrupt image
  cannot trigger a multi-gigabyte `malloc` or an unbounded loop.  (libFuzzer +
  ASan/UBSan found this class of bug before it shipped; the read path fuzzes
  clean, and `gcc -fanalyzer` / `clang --analyze` are quiet.)

## License

The original code (`v6fs.c`, `v7fs.c`, `filsysmount.c`, and their headers) is
licensed under the **ISC license**: Copyright (c) 2026 David Walther.

The `filsys.5` manpage is derived from the ancient UNIX `fs(5)` (V4, V6) and
`filsys(5)`/`dir(5)` (V7, 32V) pages, and retains the **Caldera International
"Ancient UNIX License"** (2002) notice and terms, as that license requires for
redistribution of derived documentation.

Both texts are in [`COPYING`](COPYING).
