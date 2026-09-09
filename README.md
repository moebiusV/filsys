# filsys

Mount a **Research Unix** filesystem image — the PDP-7 through Tenth Edition,
plus 32V, Coherent, early Xenix, 2.9/2.11BSD and System III/V — as a FUSE
filesystem on Linux, macOS, FreeBSD, NetBSD, or OpenBSD, so files can be copied
on and off the disk for use with a simulator (SIMH `pdp11`/`vax780`).  Linux's
own `sysv`/`v7` kernel driver was removed in 6.15 (2025) and never handled the
PDP-7 through V6, 32V, or the V8 family to begin with, so this is now the only
way to mount these filesystems — see "Linux kernel support" below.

One binary, every edition we care about: the on-disk format is understood
(middle-endian, little-endian, and big-endian byte orders, and the kernel's own
free-list and bitmap allocation disciplines), so files staged with it are seen
by a running kernel after you boot the image.

```
mount.filsys -v pdp7 pdp7.dsk mnt        # PDP-7 format, word-addressed
mount.filsys -v v1 v1root.dsk mnt        # V1 format (also V2 and V3, identical on disk)
mount.filsys -v v6 v6root.dsk mnt        # V6 format (also V4 and V5, identical on disk)
mount.filsys -v v7 rp06-0.disk mnt       # V7 format
mount.filsys -v vax32 32vroot.dsk mnt    # 32V format (V7 for the VAX, little-endian)
mount.filsys -v coherent coh.dsk mnt     # Coherent format (Mark Williams Co., V7 + interleave)
mount.filsys -v xenix xenix.dsk mnt      # early V7-derived Xenix (little-endian, 1 KB blocks)
mount.filsys -v bsd29 bsd29.dsk mnt      # 2.9BSD format (V7 inode, 1 KB blocks)
mount.filsys -v bsd211 bsd211.dsk mnt    # 2.11BSD format (32-bit inode, variable dirs)
mount.filsys -v v8 v8.dsk mnt            # Eighth Edition (1 KB free list, or 4 KB bitmap)
mount.filsys -v v9 v9.dsk mnt            # Ninth Edition (8 KB blocks, big-endian Sun-3)
mount.filsys -v v10 v10.dsk mnt          # Tenth Edition (1 KB free list, or 4 KB bitmap)
```

Home: <https://github.com/moebiusV/filsys>

## Platform support

| platform | FUSE | status |
|---|---|---|
| Linux | fuse3 (default) / fuse2 | **built + tested** (`make check`, `test.sh`) |
| Windows (WSL2) | fuse3 (Linux kernel under WSL2) | works |
| FreeBSD | fuse3 (`fusefs-libs3`) | **built + tested** |
| NetBSD ≥ 10 | fuse3 (librefuse) | **built + tested** |
| OpenBSD | fuse2 (base libfuse, 2.6-era) | **built + tested** |
| macOS | fuse3 (macFUSE ≥ 5.2) / fuse2 (macFUSE 4.x, FUSE-T) | **built + core tested** (CI); mount is local-only (macFUSE kext) |

"Built + tested" means the platform has mounted a real V7 image and passed
`test.sh` — read/write/mkdir/rename/truncate/persistence — in addition to
`make check` (the FUSE-free core).  Linux runs this locally; FreeBSD, NetBSD
11, and OpenBSD run it in CI (vmactions VMs on a real kernel) on every push.
Windows is supported through WSL2, which runs a real Linux kernel and so takes
the Linux fuse3 path.  macOS builds against macFUSE's fuse3 (via the
`fuseops_macos.c` adapter) and runs `make check` in CI, but a live mount needs
macFUSE's kernel extension, which requires a System Settings approval and
reboot that no CI runner can grant — so macOS mounts are a local-only step.
"Builds, untested" means the adapters and the configure probe compile and link,
but no live mount has been run on that platform yet.

## Dependencies

- a C17 compiler (`gcc` or `clang`)
- **libfuse3** (`libfuse3-dev` on Debian/Ubuntu, `fuse3-devel` on Fedora, `fuse3` on Arch,
  `fusefs-libs3` on FreeBSD) — the default, everywhere but OpenBSD
- **libfuse 2.x** on OpenBSD (in base, no package) — `./configure --with-fuse=fuse2`

`mkfs.filsys` and `fsck.filsys` are built here too (see "Creating and checking
filesystems" below); they need no extra dependencies beyond a C compiler.

`./configure --with-fuse=auto|fuse3|fuse2` selects the API level; `auto` (the
default) picks fuse3 where present and falls back to fuse2.  If you can't
install the dev package (no root/sysadmin), `./fetch.sh` downloads and extracts
the libfuse3 headers and static archive into a local `fuselib/` (gitignored,
never committed) and the build links against that fallback.

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
mount.filsys -v <edition> [options] <image> <mountpoint>
mount.filsys -v <edition> -c <image>   # integrity check (no mount)
```

`-v` takes the Unix edition: `pdp7` (the word-addressed PDP-7), `v1`/`v2`/`v3`,
`v4`/`v5`/`v6`, `v7`, `v8` (Eighth Edition), `v9` (Ninth Edition), `v10`
(Tenth Edition), `vax32` (32V), `coherent` (Mark Williams Co.), `xenix`
(early V7-derived Xenix), `bsd29` (2.9BSD), `bsd211` (2.11BSD), `sysiii` (System III),
`sysvr2` (System V Release 2), or `sysvr4` (System V Release 4).  A bare number — `0`,
`1`, `2`, `3`, `4`, `5`, `6`, `7`, `8`, `9`, `10`, `32`, `33`, `34`, `35`,
`36` — is also accepted, and `v0`/`p7` spell the PDP-7.  `v1`, `v2` and `v3`
are one on-disk format, and `v4` and `v5` are byte-identical to `v6`, so the
seven pre-V7 editions collapse onto two code paths.  `usgpg3` (USG Program
Generic Issue 3) resolves to `v6`, and `sysvr1` (System V Release 1) to `v7`,
both verified against real media.  The edition is **required**: there is no
default, and a wrong `-v` is an error, not a fallback.

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
| `-v vax32`| 32V format (little-endian V7)     |
| `-v coherent`| Coherent format (middle-endian V7, 64-entry free cache, interleave) |
| `-v xenix`| early V7-derived Xenix (little-endian, 1 KB blocks, 100-entry free cache, magic `0x2b5544`) |
| `-v bsd29`| 2.9BSD format (V7 inode, 1 KB blocks, 4+3 addresses) |
| `-v bsd211`| 2.11BSD format (32-bit inode, variable 63-char dirs) |
| `-v sysiii`| System III format (V7's on-disk layout) |
| `-v sysvr2`| System V Release 2/3 (s5fs: 4-byte-aligned fields + magic; byte order from `arch=`) |
| `-v sysvr4`| System V Release 4 (same layout + an `s_state` clean/dirty word) |
| `-v v8` | Eighth Edition (V7 inode, rearranged superblock; 1 KB free list or 4 KB bitmap) |
| `-v v9` | Ninth Edition (Sun-3 port: 8 KB blocks, big-endian; free list or bitmap) |
| `-v v10`| Tenth Edition (V8 + the out-of-superblock bitmap at 4 KB) |
| `-o offset=N` | mount a filesystem at byte offset N (a partition) |
| `-o arch=NAME` | override the byte order (System V on a non-default CPU) |
| `-o blocksize=N` | V8-family block size (1024/4096; 8192 for v9) |
| `-o freemap=LIST` | V8-family free space: `list` or `bitmap` (implied by block size for v8/v10) |
| `-o bitmap=SUPER\|BLOCKS` | v10 bitmap location: in-superblock (`super`) or tail blocks (`blocks`) |
| `-o byteorder=LE\|BE` | V8-family byte order (v9 is `be`; v8/v10 are `le`) |
| `-o uid=N,gid=N` | override reported ownership (default: you) |
| `-o allow_other,...` | pass a FUSE option through |
| `-r`   | mount read-only                  |
| `-f`   | stay in foreground               |
| `-d`   | FUSE debug output                |
| `-c`   | free-list / inode-table check    |

`-o arch=` (mount) and `-a arch` (mkfs/fsck) override the byte order the format
stores its multi-byte fields in.  The V7-family editions fix their own byte
order, so `arch` matters for the System V editions (`sysiii`, `sysvr2`,
`sysvr4`), which
ran on many CPUs and left byte order to the architecture.  Recognised names:

- little-endian: `vax`, `x86`, `386`, `i386`, `ns32k`, `ns32000`, `i860`, `clipper`
- big-endian: `3b2`, `3b20`, `we32000`, `68k`, `m68k`, `68000`, `sparc`, `mips`, `parisc`, `hppa`, `powerpc`, `ppc`
- PDP-11 middle-endian: `pdp11`

For the editions that carry a superblock magic word (`xenix`, `sysvr2`,
`sysvr4`), the byte order is **detected from that magic**, so a big-endian
System V volume opens (mounts or fsck's) correctly even with no `arch` at all —
the kernel does the same, reading `s_magic` in both orders before it reads
anything else.  An explicit `arch` that contradicts the magic is a hard error:

```sh
$ fsck.filsys -v sysvr2 -a vax big-endian.dk
fsck.filsys: big-endian.dk: superblock magic is big-endian, but arch 'vax' is little-endian
```

`-F` (mount) / `-F` (fsck) overrides a magic word that matches *neither* byte
order, forcing the `arch` you name (for a foreign or damaged image).

**"Xenix" names two different on-disk formats, a decade apart.**  `-v xenix` is
the *early* V7-derived Xenix (Microsoft, the PDP-11/Z8000/8086 era): magic
`0x2b5544`, 1 KB blocks, little-endian, a 100-entry free-list cache.  The later
SCO Xenix (286/386, after Xenix was rebased on System III and then System V)
stored its filesystem as the System V `s5fs` — magic `0xfd187e20`.  That is
**not a Xenix variant**; it is System V's filesystem, byte-identical to
`sysvr2`/`sysvr4`, and it mounts under those names, never under `-v xenix`.

**The V8 family** (Eighth/Ninth/Tenth Edition) is V7's inode, directory and
block-mapping engine with a *rearranged* superblock, a varying block size, a
free-space **bitmap** as an alternative to the free list, and symbolic links.
Unlike System V it carries no magic word, so its geometry is named on the
command line rather than read from disk:

- **v8** and **v10**: one device bit selects block size *and* free space
  together — 1024 ⇒ free list, 4096 ⇒ bitmap (in-superblock).  A 1 KB bitmap or
  4 KB free list cannot exist, and those combinations are rejected.
- **v9** is the **Sun-3** port: 8 KB blocks fixed, **big-endian**, and the
  free-list/bitmap choice is the one place block size does not settle it (both
  are legal at 8 KB), so `-o freemap=` may be needed.
- **v10** adds the **out-of-superblock** bitmap: when a 4 KB bitmap volume is
  too large for the 961-longword in-superblock map (data area > 30752 blocks),
  the bitmap lives in trailing blocks (`-o bitmap=blocks`).  v8 and v9 cannot
  represent it.

With no `-o`, an edition's defaults are used (`v8`/`v10` = 1 KB free list, `v9`
= 8 KB free list).  `findfs.filsys` detects the block size, byte order and
free-space form itself — see "Finding partitions" below.

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
mkfs.filsys -v v7 image.dk             # size the fs to the whole image
mkfs.filsys -v v7 image.dk 5000        # ...or to an explicit block count
mkfs.filsys -v v7 -o 18392 image.dk    # start the fs at block 18392 (a partition)
mkfs.filsys -v v7 -b /v7/mdec/rp06boot image.dk   # write a PDP-11 boot block first
mkfs.filsys -v sysvr2 -B 1024 image.dk  # a 1K-block System V filesystem
mkfs.filsys -v sysvr4 -B 2048 image.dk  # a 2K-block System V filesystem
mkfs.filsys -v v10 -g blocksize=4096,freemap=bitmap image.dk   # a 4K-block V10 bitmap filesystem
mkfs.filsys -v v10 -g blocksize=4096,freemap=bigmap image.dk   # ...with the bitmap in tail blocks

fsck.filsys -v v7 -f image.dk          # force a check of the filesystem at block 0
fsck.filsys -v v7 -o 18392 image.dk    # check a filesystem at block 18392
fsck.filsys -v v7 -p image.dk          # preen: fix the safe subset without prompting
fsck.filsys -v v7 -i image.dk          # prompt before each repair
fsck.filsys -v v10 -g blocksize=4096,freemap=bitmap image.dk   # check a bitmap filesystem
```

`mkfs.filsys` writes a superblock, a zeroed i-list, an interleaved free-block
list, and an empty root directory, laying out the root inode (and, for V7/32V,
the empty bad-block file) exactly as that edition expects — root is inode 1 in
V6, inode 2 in V7/32V and the V8 family, inode 41 in V1–V3, and inode 4 on the
PDP-7.  `-o`
places the filesystem at a block offset for multi-partition images; `-b`
installs a boot block (a PDP-11 `a.out`, V7 magic `0407`) into block 0 before
the superblock.  `-B` sets the logical block size of a System V filesystem
(`sysvr2`/`sysvr4`) to 512, 1024, or 2048 bytes — the `s_type` superblock field
— since System V is the one edition whose block size is read from the
superblock rather than fixed by the format; every other edition ignores `-B`.
The V8 family's block size and free-space form are named with `-g`
(`-g blocksize=4096,freemap=bitmap`), mirroring `mount.filsys`'s `-o` options.

`fsck.filsys` is more than the mount driver's `-c`: it folds V7's
`icheck`+`dcheck` pair into one pass (block-bitmap and duplicate detection,
free-list walk, link-count cross-check) and adds repair — `-s` rebuilds the
free list, `-r` copies out duplicate blocks (`salv -a`), `-p`/`-y` fix the safe
subset, `-i` prompts on each fix, and `-N`/`-C` are `ncheck`/`clri`.

Like the original `fsck`, a volume that already looks clean (its `s_fmod` flag
is clear) is skipped — a plain `fsck.filsys -v v7 image.dk` on a clean image
prints "filesystem clean; skipped" and does nothing.  Pass **`-f`** to force the
check regardless: `fsck.filsys -v v7 -f image.dk`.  `-g blocksize=,freemap=,byteorder=`
names the V8-family geometry the same way `mount.filsys`'s `-o` does.

These checker features are the classic **BSD `fsck`** design — the multi-phase
structure, the per-inode state byte, the bad-block/errflag handling, the
phase-1b duplicate rescan, and the `query()`/YES/NO/ASK `-y`/`-n` prompting —
which filsys folds back into its checker for *every* edition, not just Coherent:
the state byte flags an inode whose type bits name nothing recognised, the
errflag stops a badly-corrupt image from cascading into phantom missing blocks,
phase-1b names a block's first owner, and `-i`/`-y` prompt or auto-answer each
repair.

## Implementor's Notes

These are the format facts learned the hard way while building this, folded
together with the history of the filesystem itself.  They are the
documentation of record for the `pdp7fs.c` / `v1fs.c` / `v7fs.c`
backends (V6 is folded into `v7fs.c` alongside the V7/32V/Coherent/Xenix/BSD
and V8-family codecs).

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
- **V8 (Eighth Edition, 1985)**, **V9 (Ninth, 1986)**, **V10 (Tenth, 1989)** are
  the post-V7 Research line.  They keep V7's 64-byte inode (13 three-byte
  addresses) and 16-byte directory entry **byte-identical**, but rearrange the
  superblock — the free list moves into a union at the *end* (offset 248), and
  `s_fsmnt[14]`, `s_lasti` and `s_nbehind` appear — and add symbolic links
  (`IFLNK`).  Free space can be the V7 free list **or a bitmap**; the block size
  grew to 1 KB (V8/V10) and 8 KB (V9), and the free-list cache depth to 178
  (V8/V10) and 946 (V9).  **V9 is big-endian**: it is the Sun-3 (m68k) port, the
  only one whose sources survive.  V10 further adds an *out-of-superblock*
  bitmap for volumes too large for the in-superblock map.  filsys implements
  the family from the published kernel sources (`usr/src/cmd/mkfs.c`,
  `sys/sys/filsys.h`, `filsys.c`); the on-disk layout has not yet been
  cross-checked against surviving V8/V9/V10 media.

### Format table

| | PDP-7 | V1 / V2 / V3 | V4 / V5 / V6 | V7 | 32V | Coherent | Xenix (early) | 2.9BSD | 2.11BSD | V8 | V9 | V10 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| block size | 64 words (256 B) | 512 | 512 | 512 | 512 | 512 | 1024 | 1024 | 1024 | 1024 / 4096 | 8192 | 1024 / 4096 |
| inode size | 12 words (5/block) | 32 B (16/block) | 32 B (16/block) | 64 B (8/block) | 64 B (8/block) | 64 B (8/block) | 64 B (16/block) | 64 B (16/block) | 64 B (16/block) | 64 B (16/64 per block) | 64 B (128/block) | 64 B (16/64 per block) |
| block addresses | 7 words | 8 × 16-bit | 8 × 16-bit | 13 × 24-bit (3-byte packed) | 13 × 24-bit (LE) | 13 × 24-bit (ME) | 13 × 24-bit (LE) | 7 × 24-bit (ME) | 7 × 32-bit (ME) | 13 × 24-bit (LE) | 13 × 24-bit (BE) | 13 × 24-bit (LE) |
| allocator | free list | bitmap (in superblock) | free list | free list | free list | free list (interleaved) | free list (100-entry) | free list | free list | free list or bitmap | free list or bitmap | free list or bitmap (or tail-blocks) |
| file size | 56 KB | 64 KB (16-bit) | 24-bit | 32-bit | 32-bit | 32-bit | 32-bit | 32-bit | 32-bit | 32-bit | 32-bit | 32-bit |
| root inode | 4 | 41 | 1 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 |
| bad-block file | none | none | none | inode 1 | inode 1 | inode 1 | inode 1 | inode 1 | inode 1 | none | none | none |
| directory entry | 8 words | 10 B | 16 B (`d_ino` + 14-char) | 16 B | 16 B | 16 B | 16 B | 16 B | variable (≤ 63-char) | 16 B | 16 B | 16 B |

One engine covers the whole range: a `filsys_edition_t` descriptor plus a
`filsys_ops` vtable (`v7fs.c` / `filsys.c` / `filsys_format.c` / `check.c`).
What survives per edition is only what genuinely differs on disk — the inode
codec, the directory-entry codec, the block-mapping topology and the allocator —
each exposed as a vtable op.  `v1fs.c` carries V1's 32-byte inode, 10-byte
dirent and bitmap allocator; `pdp7fs.c` carries the word-addressed block codec,
the 8-word dirent and the on-disk free list; `v7fs.c` carries the V6/V7 inode and
the 2.11BSD variable-length dirent.  File read/write and path lookup are shared
across all of them: they touch only logical bytes and route block I/O through the
`blk_get`/`blk_put` data-block codec (next section).

The PDP-7 row's "block size" is its on-disk **container** (64 × 4-byte word
slots = 256 B); its **logical** block size — the `blocksize` the data layer sees
— is 128 B (64 words × two 7-bit characters).

### Block codec

The abstraction that lets a single file/dir engine serve byte- *and*
word-addressed editions is the data-block codec, `blk_get`/`blk_put` in
`filsys_ops`:

- `read_block`/`write_block` move a raw **container** block to/from the image:
  512 or 1024 bytes for the byte-addressed editions, 64 × 4-byte word slots
  (256 B) for the PDP-7.
- `blk_get`/`blk_put` move a **logical** block — `blocksize` bytes of file data,
  the `bsize` descriptor field — between that container block and a byte buffer.

For the byte-addressed editions the two coincide, so `blk_get`/`blk_put` are
literally `read_block`/`write_block`.  For the PDP-7 they differ: an 18-bit word
is two 7-bit ASCII characters packed in the low bits of each 9-bit half, so a
256-byte container block becomes 128 logical bytes.  `v7fs_file_read`/`file_write`
and `v7fs_lookup` therefore run unchanged for every edition — they only touch
logical bytes and route block I/O through `bmap` + `blk_get`/`blk_put`.

The corollary is a one-line invariant: **`bsize` is the *logical* block size, not
the on-disk footprint.**  The `open` size check `fsize × bsize ≤ image_size`
holds for the byte-addressed editions (logical == physical) but not the PDP-7,
whose on-disk footprint is `fsize × 256` and whose filesystem sits at
`base + P7_SURFACE1`; `p7fs_open` checks the container size directly rather than
deriving it from `bsize`.

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
| Xenix (early) | 16 GB (2²⁴ blocks × 1024 B) | 65,536 | ~16.1 GB (triple indirect) |
| 2.9BSD | 16 GB | 65,536 | ~16.1 GB (triple indirect) |
| 2.11BSD | 4 TB (2³² blocks × 1024 B) | 65,536 | ~16.1 GB (triple indirect) |
| V8 / V10 | 16 GB (2²⁴ blocks × 1024 B), or 64 GB (2²⁴ × 4096 B) | 65,536 (16-bit i-number) | ~16 GB (triple indirect) |
| V9 | 128 GB (2²⁴ blocks × 8192 B) | 65,536 | ~128 GB (triple indirect) |

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
  filsys handles this with a `-v vax32` selector.
- **V3-and-earlier directories are 10 bytes** (V1–V3: a 2-byte i-number and an
  8-character name), and the PDP-7's are 8 words, so the directory-entry *codec*
  differs from the 16-byte-entry V4-and-later editions.  V1 rides the shared
  codec with `dirent_size=10`/`max_namlen=8`; the PDP-7 keeps a word-based
  `dir_read`/`dir_add`/`dir_remove`.  The directory *walker* above the codec
  (`dir_lookup`) is shared across all of them.

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
  the 3-byte `di_addr` packed `[lo, mid, hi]`.  The `-v vax32` selector flips all
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
- **V8/V10** are **little-endian** (VAX/386), and **V9** is **big-endian** (the
  Sun-3/m68k port): every multi-byte field — `s_fsize`, `s_tfree`, the 3-byte
  `di_addr` packed `[lo, mid, hi]` vs `[hi, mid, lo]`, the 4-byte free-list
  `df_nfree`/`df_free[]` — follows that byte order.  The descriptor's `bo_le` /
  `bo_be` byte-order ops handle both, with `-o byteorder=` to override.

`v7fs.c` implements `balloc`/`bfree`/`ialloc`/`ifree`/`itrunc`
mirroring the respective kernel's `sys/alloc.c` (V6's and V7's free-list
discipline, and the V8-family's free-list and bitmap allocators), so the free
list stays interchangeable with what a running kernel expects.

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
    mount.filsys -v vax32 32v-rp06.disk mnt
    mount.filsys -v vax32 -o offset=9416704 32v-rp06.disk mnt/usr

    # single-filesystem images
    mount.filsys -v vax32 32v-root.disk mnt       # 32V root only
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
# found a v7, sysiii or sysvr1 filesystem at block 0 (byte 0), isize=202 fsize=5000, chain ok ...
# found a v7, sysiii or sysvr1 filesystem at block 18392 (byte 9416704), isize=8189 fsize=322278, ...
```

Mount any hit with `mount.filsys -o offset=<byte>`.

It identifies the V8 family by *traversal* rather than a magic word — it chases
the root inode, walks the i-list (which rules out the wrong byte order), and
then decides free-list vs bitmap by walking whichever representation survives
against `s_tfree`.  It reports the **equivalence class** and the geometry it
found (`found a v8 or v10, bs=1024 filesystem …`), honestly grouping the
byte-identical editions (`v1, v2 or v3`, `v4, v5, v6 or usgpg3`,
`v7, sysiii or sysvr1`); the copy-paste `mount.filsys` line underneath uses a
single canonical `-v` token (`v1`, `v6`, `v7`, `v8`, …).  By default it is
**silent about the hypotheses it discarded**; pass `-V` to see each rejected
candidate and the reason.

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
| V8 / V9 / V10 | no original media survive | — | — | synthetic images only | — | — |

On-disk verification dumped the raw 32-byte inode blocks after `chmod`/`chown`
and confirmed the mode, uid/gid and size fields landed correctly, and that
delete freed the inode and data block (free counts restored, `errors=0`).

The V8/V9/V10 rows are honest: the format is implemented from the published
kernel sources (`usr/src/cmd/mkfs.c`, `sys/sys/filsys.h`, `filsys.c`), and
`test_matrix` round-trips mkfs → write → read → truncate → fsck on synthetic
images in every geometry (1K free list, 4K bitmap, the V10 tail-blocks bitmap,
and V9's 8K big-endian forms), but no original V8/V9/V10 disk image is known to
survive, so it has not been validated against original media.

32V has no published disk image, so the test image was built from scratch:
compile open-simh's VAX-11/780 (`vax780`, which requires the `vmb.exe` ROM),
boot 32V, and install it from a tape image.

## Coordination with the simulator

> **Rule: the simulator must not run while the disk is mounted.**  The running
> kernel caches the superblock free list, the inode table, and the buffer
> cache, so editing the disk behind it leaves those stale.  The safe workflow
> is to stage files while the system is *not* running, then boot it fresh.
> (`sync` inside before halting flushes its buffers.)

mount.filsys takes an **advisory byte-range lock** over the filesystem's own
extent: a read-write mount holds an exclusive lock (an OFD lock on Linux, a
POSIX record lock on the BSDs/macOS) from `offset` to `offset + fsize*bsize`, so
a second read-write mount of the *same* filesystem fails with `already open
read-write (use -o no_lock to override)`.  Because the lock spans only the
filesystem's range, mounting the root and `/usr` partitions of one V7 disk at
two mount points is still fine: the extents are disjoint.  A read-only mount
takes no lock at all, but warns if a read-write mount already holds its range.
Pass `-o no_lock` to skip the lock (e.g. when the image is open read-write
through another handle you do not control); the lock is advisory, so it does not
stop a running emulator, which does not participate.  The rule above — do not
let the simulator run while the disk is mounted — remains the real protection
against corrupting an image the emulator has open.

## Layout

- `v7fs.h` / `v7fs.c`: the shared engine — `filsys_edition_t`, the free-list
  and bitmap allocators, and the V6/V7/32V/Coherent/Xenix/2.9BSD/2.11BSD and
  V8/V9/V10 codecs (the 2.11BSD variable-length dirent lives here too).
- `v1fs.h` / `v1fs.c`: V1/V2/V3 — the 32-byte inode, 10-byte dirent and bitmap
  allocator (its file/dir/lookup layer is shared).
- `pdp7fs.h` / `pdp7fs.c`: PDP-7 — the word container codec (`read_words` +
  `blk_get`/`blk_put`), the 8-word dirent and the on-disk free list.
- `filsys.h` / `filsys.c`: the public API and the format-independent path walker.
- `filsys_format.c`: the per-edition descriptor table (`filsys_getformat`).
- `filsys_names.c`: the edition name/alias table (`filsys_edition_by_name`).
- `filsys_ops.h`: the backend vtable.
- `blocktree.c`: the indirect-block tree walk shared by the checkers.
- `check.h` / `check.c`: the shared integrity-check driver.
- `byteorder.h` / `byteorder.c`: the byte-order ops (`bo_le`/`bo_be`/`bo_me`).
- `fuse_core.c` / `fuse_core.h`: the FUSE-free core callbacks.
- `fuseops.c` / `fuseops_macos.c` / `fuseops_openbsd.c`: the FUSE3 / macFUSE /
  OpenBSD-libfuse adapters (one is compiled per platform).
- `mount.filsys.c`: FUSE callbacks + the `-v` edition selector.
- `findfs.filsys.c`: locate filesystem superblocks (partitions) on a raw image.
- `mkfs.filsys.c`: create a filesystem of any edition in an image.
- `fsck.filsys.c`: check a filesystem of any edition (dispatches to the
  backend's `*_check()`).
- `test_matrix.c`, `test_oracle.c`: the regression and oracle tests.
- `filsys.5`, `mount.filsys.1`, `findfs.filsys.1`, `mkfs.filsys.1`,
  `fsck.filsys.1`: the format and tool manpages.
- `configure.ac`, `Makefile.am`: GNU autotools build.
- `test.sh`, `fetch.sh`.

## Notes

- Mount read-write only on a **copy** of the image; V4-V7 have no journal; a
  bug corrupts the image.
- **Durability has two boundaries, and only one is tested.**  The
  `crash_consistency` and fault-injection tests in `test_matrix` prove the
  *process* boundary: after any single injected I/O failure, or a crash before
  close, no block is both free and referenced (`dup == 0`) and `fsck.filsys -s`
  recovers the image with loss bounded to the in-flight operation.  `fsync(2)`
  on a file inside the mount (or unmounting) now `fsync`s the backing image fd,
  so a completed sync also survives *host* crash and power loss.  What is not a
  property of the format is atomicity: a V7 `creat` is eight non-atomic 512-byte
  writes across four structures, and no ordering leaves a consistent image at
  every prefix.  That is why V7 ran `sync` before halt and why `fsck` exists:
  the reachable post-crash states are a subset of those the original kernel
  could produce, and `fsck.filsys` repairs every one — a weaker claim than
  transactional consistency, and a stronger one than the tests alone establish.
- **The write path is ordered by the soft-updates rules** (McKusick & Ganger):
  never point to a structure before initialising it; never reuse a resource
  before nullifying every previous pointer to it (the aliasing invariant,
  `dup == 0`); never reset the last pointer to a live resource before setting a
  new one.  `crash_prefix_test` proves them over a bounded domain: it learns a
  bounded op's write count `W`, then re-runs the op for every prefix `k = 0..W`,
  killing the process after `k` writes, and requires every image to be
  alias-free and `fsck -s`-recoverable — complete over the operation, not a
  sample of it.
- **`mkdir` is the one non-recoverable crash.**  `mkdir` is non-monotone: its
  inode and `.`/`..` land before the parent's entry (rule 1, correctly), so a
  crash between them orphans the new directory.  `fsck` cannot auto-repair an
  orphan *directory* without a `lost+found` phase, so `crash_prefix_test` covers
  the recoverable ops, and `mkdir`'s error path (where the rollback runs) is
  covered by fault injection.  `dup == 0` still holds at every `mkdir` prefix —
  the limit is recoverability, not aliasing.
- **`property_sequences` drives random operation histories** against the library
  and a small in-memory model, then requires the image to be fsck-clean and every
  surviving file to read back at its modelled size.  A hand-written matrix only
  finds bugs someone thought to write a case for; this finds a different class.
- The `-c` integrity check walks the free list and the inode table and
  reports out-of-range block numbers, cycles, and unreadable inodes.
- The triple-indirect path is exercised by `v7_triple_indirect` in
  `test_matrix`: a 9 MiB write crosses the double-indirect boundary (16522
  blocks) and forces `di_addr[12]`, then reads back and fsck's clean.
- The on-disk `fsize` and every inode's `size` are validated against the real
  image size and the data area **before any allocation**, so a corrupt image
  cannot trigger a multi-gigabyte `malloc` or an unbounded loop.  (libFuzzer +
  ASan/UBSan found this class of bug before it shipped; the read path fuzzes
  clean.  `clang --analyze` is quiet; `gcc -fanalyzer` reports one false
  positive — it cannot see through the `fs->io->read` / `fs->word->get`
  indirection in `pdp7fs.c`'s `read_words` to prove all 64 slots are filled.)

`make check` exercises the FUSE-free core (the format codecs, allocators and
checker) through `test_matrix`; it depends on `all`, which also builds
`mount.filsys`, so a link against libfuse is still needed even to run
`make check` alone.  To run the same suite without any FUSE on the link path,
build the test binary directly — `make test_matrix && ./test_matrix` — which
links only `libfilsys.a` (the FUSE-free core) and never touches `mount.filsys`.

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

### System V: one s5fs layout, and three places the Linux header misleads

The obvious reference for the System V on-disk format is the kernel's own
`include/linux/sysv_fs.h`, and it is wrong in three places — all three caught
only by checking against the AT&T sources (`filsys.h` in the 3B2/32000 System V
R2 tree, `s5filsys.h` in the i386 R4 tree, both on archive.org) and by booting
real media in SIMH.

1. **There is no `s_pad2`.**  `struct sysv4_super_block` puts a 2-byte pad
   between `s_ninode` and `s_inode`, pushing `s_inode` to offset 216.  The real
   struct has none: `ino_t` is `ushort`, so `s_inode` is naturally aligned at
   214.  System V R2 through R4 share one 4-byte-aligned layout — `daddr_t` and
   `time_t` aligned to 4, `s_inode` at 214, `s_time` at 420, `s_dinfo[4]` at
   424, `s_tfree` at 432, `s_magic` at 504, `s_type` at 508.

2. **`s_state` clean is `0x7c269d38`, not `0xcb096f43`.**  `0xcb096f43` is
   `FsBAD` (bad root).  Clean is `FsOKAY = 0x7c269d38`; mounted/dirty is
   `FsACTIVE = 0x5e72d81a`.  They are constants.

3. **`s_state` is not time-derived.**  The kernel compares `s_state` against
   `0x7c269d38 - s_time` — a detection heuristic for telling a pre-1980 volume
   from a later one, not a value any writer stores.  `s_state` is written as-is,
   and it is an R4 field only (R2/R3 leave that word as `s_fill[12]`).

A fourth finding, this one about System III rather than the header: **the System
III precursor is V6**.  Booting the Cloutier USG PG3 tape (tuhs.org) in SIMH and
running filsys's own `fsck` over its root filesystem reports V6 layout — 16-bit
block numbers, 32-byte inodes — clean.  System III proper (1981) is still
unconfirmed, but its direct predecessor is V6, not V7.  PDP-11 System V Release
1 is V7 with no magic at all; the `0xfd187e20` s5fs magic only appears on the
32-bit ports (VAX/3B2/68k) from Release 2 on.

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

On the one thing that decides whether a large file reads back, the block mapping,
filsys and the kernel were **at parity, not ahead of each other**: both descend
the full V7 triple-indirect chain (10 direct + single + double + triple).  filsys
walks all three indirect levels in `v7fs_bmap`; the kernel's `fs/sysv/itree.c`
`block_to_path` is `DIRECT=10, DEPTH=4` ("Have triple indirect"), with the
triple slot at `di_addr[12]`.  The port that is genuinely short of this is
**plan9port's `v10fs`**, which is single-indirect only (its own comment: "only
singly-indirect files for now"; it treats slots 10/11/12 as three consecutive
single indirects) and also uses the wrong mode constants (`VFMT 0160000` rather
than `IFMT 0170000`) — so a file past roughly 4 MB reads garbage and a symlink
masks to its character-device test and is lost.

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

The original code (`pdp7fs.c`, `v1fs.c`, `v7fs.c`, `filsys.c`,
`filsys_format.c`, `filsys_names.c`, `blocktree.c`, `byteorder.c`, `check.c`,
`fuse_core.c`, `mount.filsys.c`, `findfs.filsys.c`, `mkfs.filsys.c`,
`fsck.filsys.c`, the FUSE adapters `fuseops.c` / `fuseops_macos.c` /
`fuseops_openbsd.c`, the test drivers `test_matrix.c` / `test_oracle.c`, and
their headers) is licensed under the **ISC license**: Copyright (c) 2026 David
Walther.

The `filsys.5` manpage is derived from the ancient UNIX `fs(5)` (V4, V6) and
`filsys(5)`/`dir(5)` (V7, 32V) pages, and retains the **Caldera International
"Ancient UNIX License"** (2002) notice and terms, as that license requires for
redistribution of derived documentation.

Both texts are in [`COPYING`](COPYING).
