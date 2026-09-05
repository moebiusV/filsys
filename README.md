# filsys

Mount a **Research Unix** filesystem image (PDP-11) as a FUSE filesystem on
Linux, so files can be copied on and off the disk for use with a simulator
(SIMH `pdp11`).  Linux's own `sysv`/`v7` kernel driver was removed in 6.15
(2025) and never handled the PDP-7 through V6 or 32V to begin with, so this is now the
only way to mount these filesystems — see "Linux kernel support" below.

One binary, every edition we care about: the on-disk format is understood
(middle-endian byte order and the kernel's own free-list allocation
discipline), so files staged with it are seen by a running kernel after you
boot the image.

```
mount.filsys -v pdp7 pdp7.dsk mnt        # PDP-7 format, word-addressed
mount.filsys -v v1 v1root.dsk mnt        # V1 format (also V2 and V3, identical on disk)
mount.filsys -v v6 v6root.dsk mnt        # V6 format (also V4 and V5, identical on disk)
mount.filsys -v v7 rp06-0.disk mnt       # V7 format
mount.filsys -v 32v 32vroot.dsk mnt      # 32V format (V7 for the VAX, little-endian)
mount.filsys -v coherent coh.dsk mnt     # Coherent format (Mark Williams Co., V7 + interleave)
```

Home: <https://github.com/moebiusV/filsys>

## Dependencies

- a C17 compiler (`gcc` or `clang`)
- **libfuse3** (`libfuse3-dev` on Debian/Ubuntu, `fuse3-devel` on Fedora, `fuse3` on Arch)

`mkfs.filsys` and `fsck.filsys` are built here too (see "Creating and checking
filesystems" below); they need no extra dependencies beyond a C compiler.

If you can't install the dev package, `./fetch.sh` downloads and extracts the
libfuse3 headers locally and the build falls back to linking the runtime
SONAME directly.

## Build

```sh
./configure
make
sudo make install    # installs mount.filsys, findfs.filsys, mkfs.filsys, fsck.filsys + manpages
```

`./configure && make && make install` is the standard GNU flow; `configure` is
shipped so no autotools are needed.  (To build from a git checkout after
editing `configure.ac` or `Makefile.am`, run `autoreconf -i` first.)  The code
is compiled as C17 (not C23) to match Microsoft's toolchain ceiling.

## Usage

```sh
mount.filsys -v <pdp7|v1|v2|v3|v4|v5|v6|v7|32v|coherent> [options] <image> <mountpoint>
mount.filsys -v <pdp7|v1|v2|v3|v4|v5|v6|v7|32v|coherent> -c <image>   # integrity check (no mount)
```

`-v` takes the Unix edition: `pdp7` (the word-addressed PDP-7), `v1`/`v2`/`v3`,
`v4`/`v5`/`v6`, `v7`, `32v`, or `coherent` (Mark Williams Co.; a bare number —
`0`, `1`, `2`, `3`, `4`, `5`, `6`, `7`, `32` — is also accepted, and `v0`/`p7`
spell the PDP-7).  `v1`, `v2` and `v3` are one on-disk format, and `v4` and `v5`
are byte-identical to `v6`, so the seven pre-V7 editions collapse onto two code
paths.

| option | meaning                          |
|--------|----------------------------------|
| `-v pdp7` | PDP-7 format, word-addressed |
| `-v v1` | V1 format (also V2/V3, identical on disk) |
| `-v v2` | V2 format (identical to V1/V3)   |
| `-v v3` | V3 format (identical to V1/V2)   |
| `-v v4` | V4 format (byte-identical to V5/V6) |
| `-v v5` | V5 format (byte-identical to V4/V6) |
| `-v v6` | V6 format                          |
| `-v v7` | V7 format                          |
| `-v 32v`| 32V format (little-endian V7)      |
| `-v coherent`| Coherent format (middle-endian V7, 64-entry free cache, interleave) |
| `-o offset=N` | mount a filesystem at byte offset N (a partition) |
| `-o uid=N,gid=N` | override reported ownership (default: you) |
| `-o allow_other,...` | pass a FUSE option through |
| `-r`   | mount read-only                  |
| `-f`   | stay in foreground               |
| `-d`   | FUSE debug output                |
| `-c`   | free-list / inode-table check    |

```sh
mkdir mnt
mount.filsys -v v7 rp06-0.disk mnt        # read-write (make a copy first!)
ls mnt
cp mnt/etc/passwd .             # copy a file off
cp host.txt mnt/tmp/            # copy a file on
fusermount3 -u mnt              # unmount

mount.filsys -v v6 -c v6root.dsk          # verify the free list + inode table
```

See `filsys.5` for both the tool and the on-disk format.

## Creating and checking filesystems

Two small tools ship alongside the mount driver for creating and checking a
filesystem of any edition on a disk image (`fsck.filsys` runs the same
free-list/inode-table check that the mount driver's `-c` does):

```sh
mkfs.filsys image.dk             # size the fs to the whole image
mkfs.filsys image.dk 5000        # ...or to an explicit block count
mkfs.filsys -o 18392 image.dk    # start the fs at block 18392 (a partition)
mkfs.filsys -b /v7/mdec/rp06boot image.dk   # write a PDP-11 boot block first

fsck.filsys image.dk             # check the filesystem at block 0
fsck.filsys -o 18392 image.dk    # check a filesystem at block 18392
fsck.filsys -p image.dk          # preen: fix the safe subset without prompting
fsck.filsys -i image.dk          # prompt before each repair
```

`mkfs.filsys` writes a superblock, a zeroed i-list, an interleaved free-block
list, and an empty root directory, laying out the root inode (and, for V7/32V,
the empty bad-block file) exactly as that edition expects — root is inode 1 in
V6, inode 2 in V7/32V, inode 41 in V1–V3, and inode 4 on the PDP-7.  `-o`
places the filesystem at a block offset for multi-partition images; `-b`
installs a boot block (a PDP-11 `a.out`, V7 magic `0407`) into block 0 before
the superblock.

`fsck.filsys` is more than the mount driver's `-c`: it folds V7's
`icheck`+`dcheck` pair into one pass (block-bitmap and duplicate detection,
free-list walk, link-count cross-check) and adds repair — `-s` rebuilds the
free list, `-r` copies out duplicate blocks (`salv -a`), `-p`/`-y` fix the safe
subset, `-i` prompts on each fix, and `-N`/`-C` are `ncheck`/`clri`.

These checker features are the classic **BSD `fsck`** design — the multi-phase
structure, the per-inode state byte, the bad-block/errflag handling, the
phase-1b duplicate rescan, and the `query()`/YES/NO/ASK `-y`/`-n` prompting —
which Coherent's own `fsck` (a BSD descendant) also carries.  We read them out
of Coherent's BSD-licensed `fsck` source (see the Acknowledgments) and folded
them back into this checker for *every* edition, not just Coherent: the state
byte flags an inode whose type bits name nothing recognised, the errflag stops
a badly-corrupt image from cascading into phantom missing blocks, phase-1b
names a block's first owner, and `-i`/`-y` prompt or auto-answer each repair.
Coherent is the newest format here, but reading its checker improved the
oldest ones.

## Implementor's Notes

These are the format facts learned the hard way while building this, folded
together with the history of the filesystem itself.  They are the
documentation of record for the `pdp7fs.c` / `v1fs.c` / `v6fs.c` / `v7fs.c`
backends.

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
- **V1-V3** (1971-73) kernels are PDP-11 **assembly**, and all three share one
  on-disk format: **10-byte** directory entries (2-byte i-number + 8-char name),
  a **bitmap** free-block/inode allocator, a 32-byte inode, device files marked
  by i-numbers below 41 (the root is inode 41), and times in 60ths of a second.
  The i-list, directories-as-files, and device files were already there from the
  1969 design.  Note that no original V1 media survives — what filsys reads is
  the *reconstructed* V1-era format, verified against Yufeng Gao's mid-1972
  "V2 beta" RF image rebuilt from Dennis Ritchie's s1/s2 DECtapes (see
  Acknowledgments); the bytes on a real 1971 V1 pack may have differed in ways
  no longer observable.
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
- **Coherent** (Mark Williams Co.) is V7 with three small changes: a 64-entry
  free-list cache (V7 has 50), an `s_unique` superblock field, and an
  `s_m`/`s_n` cylinder interleave applied when the free list is built.  Its
  byte order is the PDP-11's middle-endian — the format was fixed on the PDP-11
  and preserved verbatim on x86 — so it rides the V7 code path, not 32V's.
  See `docs/coherent-format.md`.

### Format table

| | PDP-7 | V1 / V2 / V3 | V4 / V5 / V6 | V7 | 32V | Coherent |
|---|---|---|---|---|---|---|
| block size | 64 words (256 B) | 512 | 512 | 512 | 512 | 512 |
| inode size | 12 words (5/block) | 32 B (16/block) | 32 B (16/block) | 64 B (8/block) | 64 B (8/block) | 64 B (8/block) |
| block addresses | 7 words | 8 × 16-bit | 8 × 16-bit | 13 × 24-bit (3-byte packed) | 13 × 24-bit (LE) | 13 × 24-bit (ME) |
| allocator | free list | bitmap (in superblock) | free list | free list | free list | free list (interleaved) |
| file size | 56 KB | 64 KB (16-bit) | 24-bit | 32-bit | 32-bit | 32-bit |
| root inode | 4 | 41 | 1 | 2 | 2 | 2 |
| bad-block file | none | none | none | inode 1 | inode 1 | inode 1 |
| directory entry | 8 words | 10 B | 16 B (`d_ino` + 14-char) | 16 B | 16 B | 16 B |

Four code paths cover the whole range: `pdp7fs.c` (`-v pdp7`, word-addressed),
`v1fs.c` (`-v v1`/`v2`/`v3`, one bitmap format), `v6fs.c` (`-v v4`/`v5`/`v6`,
byte-identical on disk), and `v7fs.c` (`-v v7`/`32v`/`coherent`, the byte orders
and free-cache widths of one format).

### Limits

On-disk architectural maxima — what the format can address, not what a given
disk image holds:

| edition | max filesystem size | max files (inodes) | max file size |
|---|---|---|---|
| PDP-7 | 64 MB (2¹⁸ blocks × 256 B) | 262,144 (18-bit i-number) | 56 KB (7 × 64 × 64 words) |
| V1 / V2 / V3 | 6528 blocks × 512 B (~3.3 MB) | 65,536 (16-bit i-number) | 64 KB (16-bit size field) |
| V4 / V5 / V6 | 32 MB | 65,536 | 1 MB (8 single-indirect × 256 blocks) |
| V7 | 8 GB (2²⁴ blocks × 512 B) | 65,536 | ~1.08 GB (triple indirect) |
| 32V | 8 GB | 65,536 | ~1.08 GB |
| Coherent | 8 GB | 65,536 | ~1.08 GB |

The PDP-7's real RB09 disk held only 8000 blocks (2 MB) per surface; 64 MB is
the 18-bit block-number ceiling.  V1's 16-bit block numbers could address 32 MB,
but its free-block and inode bitmaps live *inside the two-block superblock*, so
a V1 volume is capped at 6528 blocks (~3.3 MB) — the bitmaps cannot fit past
that.  V1's 16-bit size field separately caps a file at 64 KB even though the
large-file flag can address a megabyte of blocks.

### Gotchas

- **The V6 24-bit size is `(size0 << 16) | size1`.**  `size0` (one byte at
  inode offset +5) is the *high* byte and `size1` (the word at +6) is the
  *low* 16 bits, not the other way around.  Getting this backwards makes a
  160-byte root directory read as 40960 and every write fail `ENOSPC`.
- **Root inode differs by edition.**  V4/V5/V6 have `ROOTINO 1` and no
  bad-block file; V7/32V have `ROOTINO 2` and reserve inode 1.  Do not carry
  the V7 "root is 2 / inode 1 is bad blocks" convention back to V6.
- **`s_isize` means two different things.**  In both editions block 0 is the
  boot block and block 1 is the superblock, but what the superblock's `isize`
  field *counts* changed silently between V6 and V7.  V6's `s_isize` is the
  **number of i-list blocks**, so the i-list is blocks `2..s_isize+1` and the
  first data block is `s_isize+2` (the V6 kernel's `ialloc` reads block `i+2`
  for `i < s_isize`, and `badblock` rejects `bn < s_isize+2`).  V7's `s_isize`
  is the **first data block**, so the i-list is blocks `2..s_isize-1`.  A tool
  that reads `isize` with the wrong interpretation is silently off by two
  blocks at the high end of the i-list — exactly the sort of thing that only
  shows up on a full disk.
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
- **The device number is `(major << 8) | minor`, and `mknod` stages `/dev`.**
  V7 packs an 8-bit major and 8-bit minor into one 16-bit word —
  `makedev(x,y) = (x<<8)|y` in `/usr/include/sys/types.h` — and that word is
  what sits in `i_addr[0]` (`/dev/tty0` is `0x300` = major 3, minor 0; `/dev/rp3`
  is `0x607` = major 6, minor 7).  `mount.filsys` implements the `mknod`
  operation, so a bootable `/dev` can be staged by running `mknod` on the
  mounted image: the driver re-encodes the host's `makedev(major, minor)` into
  V7's `(major<<8)|minor` and drops it in `i_addr[0]`, leaving the rest of
  `i_addr` zero exactly as V7's `mknod` does.  (The host needs `CAP_MKNOD`, so
  stage as root.)
- **Hard-linking a directory was legal in V7 — and modern Linux won't let a FUSE
  driver even try.**  V7's `link(2)` guarded directory links with
  `if ((ip->i_mode & IFMT) == IFDIR && !suser())`: the superuser could hand a
  directory a second name, and the filesystem recorded it faithfully — nlink
  bumped, `..` *not* rewritten, so the linked directory still points at its
  original parent, which is exactly how you built a cycle.  It was a famous
  footgun; `find`, `fsck`, and the dump/restore tools had no defence against a
  directory cycle until later editions hardened `link()` against directories.
  `mount.filsys` stays faithful: its `link` operation permits directory links.
  But on a Linux host you cannot actually exercise it — the kernel's
  `vfs_link()` returns `EPERM` for `S_ISDIR` *before* a FUSE filesystem's
  `link` callback is ever reached.  The permissiveness survives in the driver
  as archaeology, even where the host platform has since closed the door.
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
  filsys handles this with a `-v 32v` selector.
- **V3-and-earlier directories are 10 bytes** (V1–V3: a 2-byte i-number and an
  8-character name), and the PDP-7's are 8 words, so readers for those editions
  use a different directory walker than the 16-byte-entry V4-and-later
  editions.

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
  the 3-byte `di_addr` packed `[lo, mid, hi]`.  The `-v 32v` selector flips all
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
(`18392 x 512 = 9416704`): `mount.filsys -v v7 -o offset=9416704 rp06-0.disk mnt`,
and `-c` reports 2064 used inodes, `errors=0`; it mounts as a complete
May-1979 source tree (`/usr/src`, `/usr/sys`, man pages, games).

To locate such a partition you read `/etc/rc` (for the *name*), read the
driver's partition table (for the *offset*), or run **`findfs.filsys`** (see
below), which scans for superblocks at cylinder boundaries and, with `-i`,
traces inode-table runs backwards to their superblocks.  Do not cap `isize`
too low: this `/usr` has `isize=8189` (65,512 inodes), which a naive
"small i-list" heuristic wrongly skips.

`mount.filsys -o offset=N` shifts the superblock read to byte `N`, so a
partition mounts in place without `dd`, and the root and `/usr` partitions
can be mounted from the *same* file at once, nested:

```
mount.filsys -v v7 rp06-0.disk mnt/
mount.filsys -v v7 -o offset=9416704 rp06-0.disk mnt/usr
```

The second (nested) mount works because mount.filsys reports files as the
mounting user (override with `-o uid=,gid=`), so the inner mount point is
owned by you.

### Mount commands

Copy-paste commands per image (images distributed by the
[prebsd](https://github.com/moebiusV/prebsd) project):

    # V7 (rp06-0.disk): root 0-4999, swap 5000-18391, /usr 18392+
    mount.filsys -v v7  rp06-0.disk mnt
    mount.filsys -v v7  -o offset=9416704 rp06-0.disk mnt/usr

    # 32V (32v-rp06.disk): same layout as V7
    mount.filsys -v 32v 32v-rp06.disk mnt
    mount.filsys -v 32v -o offset=9416704 32v-rp06.disk mnt/usr

    # single-filesystem images
    mount.filsys -v 32v 32v-root.disk mnt       # 32V root only
    mount.filsys -v v6  rk0 mnt                 # V6 root only

Mount the root first, then nest the `/usr` mount on top.

### Finding partitions (findfs.filsys)

`findfs.filsys` locates the filesystems on a raw image.  It scans for superblocks
(validating the edition, i-list and volume sizes, and that the free list holds
only in-range blocks), and with `-i` also scans for inode-table runs and
traces backwards to their superblocks.  Scan at cylinder boundaries to dodge
the false positives a block-by-block sweep of file data produces:

```
findfs.filsys -s 418 rp06-0.disk
# fs @ block 0      (byte 0)       V7  isize=202  fsize=5000
# fs @ block 18392  (byte 9416704) V7  isize=8189 fsize=322278
```

Mount any hit with `mount.filsys -o offset=<byte>`.

## Verification

The read path and the write path were both exercised against real images of
the V4-through-32V editions.  The earlier editions have no original media
surviving, so the PDP-7 layout is verified against the pdp7-unix
reconstruction and V1–V3 against Yufeng Gao's "V2 beta" RF image; the
create/write/delete path for every edition is additionally run by
`test_matrix` under `make check`:

| edition | image | `-c` | mount | create/write | chmod/chown | delete |
|---|---|---|---|---|---|---|
| PDP-7 | pdp7-unix reconstruction | yes | yes | yes | yes | yes |
| V1 / V2 / V3 | Gao's `V2 beta` RF (reconstruction) | yes | yes | yes | yes | yes |
| V4 | TUHS `Utah_v4/disk.rk` | yes | yes | yes | yes (on-disk bytes verified) | yes |
| V5 | TUHS `Dennis_v5/v5root` | yes | yes | yes | yes | yes |
| V6 | pcollinson `rk0` / SIMH `uv6swre` | yes | yes | yes | yes | yes |
| V7 | pcollinson `rp06-0.disk` | yes | yes | yes | yes | yes |
| 32V (VAX) | `32v-root.disk`, `32v-rp06.disk` (`/usr`) | yes | yes | yes | yes | yes |
| Coherent | `disk1..4.4.10.dd` (PUPS base floppies) | yes | — | — | — | — |

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

mount.filsys deliberately takes **no lock** on the image: a V7 disk is a set of
partitions in one file, and mounting the root and `/usr` at two mount points
from the same file at once requires both mounts to share it read-only.  The
"don't edit a disk under a running kernel" rule above is the real protection;
the file is never locked, so it is on you not to mount read-write while the
emulator is running.

## Layout

- `pdp7fs.h` / `pdp7fs.c`: PDP-7 word-addressed on-disk access layer.
- `v1fs.h` / `v1fs.c`: V1/V2/V3 on-disk access layer.
- `v6fs.h` / `v6fs.c`: V4/V5/V6 on-disk access layer.
- `v7fs.h` / `v7fs.c`: V7/32V on-disk access layer.
- `filsys.h` / `filsys.c`: the shared `filsys_ops` table and the
  format-independent path walker.
- `mount.filsys.c`: FUSE callbacks + the `-v` edition selector.
- `findfs.filsys.c`: locate filesystem superblocks (partitions) on a raw image.
- `mkfs.filsys.c`: create a filesystem of any edition in an image.
- `fsck.filsys.c`: check a filesystem of any edition (dispatches to the
  backend's `*_check()`).
- `filsys.5`, `mount.filsys.1`, `findfs.filsys.1`, `mkfs.filsys.1`,
  `fsck.filsys.1`: the format and tool manpages.
- `configure.ac`, `Makefile.am`: GNU autotools build.
- `test.sh`, `fetch.sh`.

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

## Linux kernel support

A short history of how the mainline kernel handled (and then stopped handling)
these filesystems, and why filsys is a FUSE driver rather than a kernel module.

### The driver was removed in 6.15

The `sysv`/`v7` driver had been orphaned since 2023 with nobody willing to
maintain it, and Jan Kara's removal patch landed in the VFS branch for the 6.15
merge window.  The commit is `sysv: Remove the filesystem` (2025-02-21),
dropping ~3.4k lines.

The rationale is worth reading in full, because it bears directly on this
project:

> Since 2002 (change "Replace BKL for chain locking with sysvfs-private rwlock")
> the sysv filesystem was doing IO under a rwlock in its get_block() function
> (yes, a non-sleepable lock hold over a function used to read inode metadata
> for all reads and writes).  Nobody noticed until syzbot in 2023.  This shows
> nobody is using the filesystem.  Just drop it.

Twenty-three years of sleeping under a spinlock on every read and write,
discovered by a fuzzer rather than a user.  The last kernel with it is 6.14; the
driver registry confirms `fs/sysv/super.c` covering 2.5.45 through 6.14 for both
the `sysv` and `v7` type names.

### What it supported: V7 only

`CONFIG_SYSV_FS` registered two filesystem types from one driver.  `-t sysv`,
`-t xenix`, and `-t coherent` were interchangeable names for the SysV family;
`-t v7` was a separate `file_system_type` for Seventh Edition.

V6, V5, and V4 were never supported.  The layouts differ in ways the driver had
no code for — `NICFREE` is 100 rather than 50, the inode is 32 bytes with 8
`addr[]` entries rather than 64 with 40, and `s_isize` counts something
different.  32V was never supported either, for the reason the rest of this
README makes so much of: the 32-bit fields are middle-endian, and the driver's
`fs32_to_cpu` only handled straight LE and BE.

### How it told them apart: magic for SysV, guesswork for V7

Xenix, SysV, and Coherent each have a superblock magic (`0x2b5544`, `0xfd187e20`,
and a `s_fname`/`s_fpack` check respectively), tried in sequence with
byte-swapped variants to determine endianness.

V7 has no magic number at all, so `v7_sanity_check()` guessed.  Roughly:

- superblock plausibility — `s_nfree <= 50`, `s_ninode <= 100`, `s_fsize` under
  the V7 maximum;
- then read block 2 and inspect the root inode at offset 64: it must be a
  directory, non-zero size, size a multiple of 16, and no larger than
  `V7_NFILES` entries.

Because that's a heuristic rather than a magic check, `v7` was never in the
autodetect chain.  You had to name it explicitly with `-t v7`, or nothing
happened — and the heuristic false-negatived on real disks (Lubomir Rintel's
2010 commit is literally titled "fs/sysv: v7: adjust sanity checks for some
volumes").

The detection story outlived the driver only partway.  libblkid still probes
`sysv` and `xenix`, but there is no `v7` prober — same reason: nothing to match
on.

### mkfs and fsck: never existed

Not removed — never written.  util-linux ships exactly these:

```
/sbin/mkfs.{bfs,cramfs,ext2,ext3,ext4,minix}
/sbin/fsck.{cramfs,ext2,ext3,ext4,minix}
```

No `mkfs.sysv`, no `fsck.sysv`, no `v7` variants.  fsck(8)'s own SEE ALSO lists
ext2/ext3, cramfs, jfs, nfs, minix, msdos, vfat, xfs and reiserfsck — nothing in
the family.  So even in 6.14 you could mount a V7 image read-write with a driver
carrying a 23-year-old locking bug, and had no way to create one or check one.

### What this means for filsys

Mainline ever handled **one** edition — V7 — guessed at it, couldn't create it,
couldn't check it, and dropped it in 6.15.  filsys handles the PDP-7
through 32V, has `findfs` for locating a superblock on a raw image, and has
both `mkfs` and `fsck`.  That is not an incremental improvement on what the
kernel had — it is the only implementation that exists.

Two things follow.

**The removal rationale is the argument for FUSE.**  What got `sysv` killed was
in-kernel complexity nobody could justify maintaining for a handful of users — a
sleeping-under-spinlock bug only a fuzzer would find.  A userspace FUSE driver
has no `get_block()`, no BKL legacy, no locking contract with the VFS, and
cannot wedge a kernel when it hits a corrupt superblock.  It can only be wrong
in ways that hurt the person who ran it.  That is the correct place for a
filesystem with maybe two hundred users worldwide, and it is why a FUSE version
can survive where the kernel's could not.

**But take the warning too.**  The proximate cause of death was a fuzzer finding
the bug first when no human had in twenty-three years.  The read path here fuzzes
clean under libFuzzer and ASan (see "Notes"); extending that to `fsck` and to
liveness assertions — not just sanitizer trips — is cheap insurance against
being the author of the *second* Research Unix filesystem implementation that a
fuzzer had to audit.

## Acknowledgments

filsys does not so much read filesystems as read the work of a handful of
people who built them and a smaller handful who saved them from a notebook in a
box.  We wrote none of the on-disk formats, recovered none of the listings,
typed none of the assembly, and built none of the images this project mounts;
that labour was all done by others, and the debt is total.

The formats themselves are the work of **Kenneth Lane Thompson**, **Dennis
MacAlistair Ritchie**, and **Rudd Canaday**, who designed the filesystem in
1969 and wrote it first for the word-addressed PDP-7 and then across V1–V3 in
hand-assembled PDP-11 code.  Ritchie's own account is "The Evolution of the
Unix Time-sharing System".

The reason there is anything left to read is a small group of modern
restorationists who did the unglamorous, painstaking work of lifting 1969–1973
source off paper and tape into a form we could study:

- **Norman Wilson** made the original scans of the PDP-7 (and V1) assembly
  listings while he was at Bell Labs.  There is no PDP-7 source — and no format
  to reverse-engineer — without those scans.
- **Warren Toomey** received Dennis Ritchie's original V1–V3 DECtapes in 1997,
  founded **The Unix Heritage Society (TUHS)** to preserve all of it, led the
  *pdp7-unix* resurrection, typed the barely-legible scans into assembler
  source, and wrote the `mkfs7`/`fsck7` Perl tools whose constants are the
  authoritative record of the PDP-7 on-disk format this backend follows.
- **Angelo Papenhoff** analysed the Dennis Ritchie DECtapes that Toomey had held
  back, recovering V2–V4 binaries and identifying the `NB` intermediate
  language, and published the tape contents on TUHS.
- **Yufeng Gao** rebuilt the mid-1972 "V2 beta" system from the s1/s2 tapes;
  its RF disk image is the surviving machine-readable reference against which
  the V1–V3 on-disk format was verified here.
- **Phil Budne** coaxed the restored PDP-7 kernel up to a login prompt, wrote
  the RIM bootstrap that boots it on real hardware, and fixed transcription
  errors in the shell and `ed`.
- **Robert Swierczek** made the B compiler self-hosting on the restored system.
- **Dennis Ritchie** personally preserved and donated the DECtapes and source
  that became the `Dennis_v1` archive, without which V1–V3 would be a gap.
- The **Living Computer Museum** ran the restored system on a real PDP-7,
  proving the reconstruction faithful.

The V6 and V7 disk images that filsys verifies every edition against came from
two more people, further down the preservation chain:

- **Keith Bostic** supplied the V7 tape now preserved as TUHS's
  *Keith_Bostic_v7* archive — the tape from which the V7 test image was
  unpacked.
- **Paul Collinson** (the `pcollinson` of the `unixv6-extras` and
  `unixv7-extras` projects) unpacked that tape into the `rp06-0.disk` image and
  built the V6 `rk0` image, then hosted them so a filesystem can be fetched with
  a single `curl` instead of a tape drive.

The Coherent filesystem is the work of **Mark Williams Company** (MWC), which
sold Coherent as a commercial Unix clone from 1980 into the 1990s.  **Robert
"Bob" Swartz**, MWC's founder and president, agreed on 3 January 2015 to release
the Coherent command and system sources under the **3-clause BSD license**;
that dump — source, some RCS history, tarballs, and a version-4 binary
distribution — is published at `nesssoftware.com/home/mwc/source.php`, mirrored
on the Internet Archive as `mwc-coherent-unix-clone`, and on GitHub as
`gspu/Coherent`.  It is that release that makes the Coherent backend here
possible and freely implementable.

Every format table in this document was read out of code, tape, or image that
these people recovered or released; our project would not exist without their
work.

- pdp7-unix restoration: <https://github.com/DoctorWkt/pdp7-unix>
- Norman Wilson's scans: <https://www.tuhs.org/Archive/Distributions/Research/McIlroy_v0/>
- The V1–V3 archive: <https://www.tuhs.org/Archive/Distributions/Research/Dennis_v1/>
- Keith Bostic's V7 tape: <https://www.tuhs.org/Archive/Distributions/Research/Keith_Bostic_v7>
- Paul Collinson's V6/V7 images: <https://github.com/pcollinson/unixv7-extras>
- The Unix Heritage Society: <https://www.tuhs.org/>
- Coherent source (Robert Swartz's 2015 BSD-licensed release): <http://www.nesssoftware.com/home/mwc/source.php>
- Coherent source mirror (GitHub): <https://github.com/gspu/Coherent>
- Ritchie's history: <https://www.bell-labs.com/usr/dmr/www/hist.html>

## License

The original code (`pdp7fs.c`, `v1fs.c`, `v6fs.c`, `v7fs.c`, `filsys.c`,
`mount.filsys.c`, `findfs.filsys.c`, `mkfs.filsys.c`, `fsck.filsys.c`, and
their headers) is licensed under the **ISC license**: Copyright (c) 2026 David
Walther.

The `filsys.5` manpage is derived from the ancient UNIX `fs(5)` (V4, V6) and
`filsys(5)`/`dir(5)` (V7, 32V) pages, and retains the **Caldera International
"Ancient UNIX License"** (2002) notice and terms, as that license requires for
redistribution of derived documentation.

Both texts are in [`COPYING`](COPYING).
