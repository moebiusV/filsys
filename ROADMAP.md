# Roadmap

filsys is at 4.1.2.  Version numbers follow [Semantic Versioning](https://semver.org/)
per [VERSIONING.md](VERSIONING.md).

## Shipped

- Research Unix filesystems from PDP-7 (V0) through V10, plus 32V, Coherent,
  Xenix, 2.9BSD, 2.11BSD, System III, and System V (s5fs) — `mount`, `fsck`,
  `mkfs`, and `findfs`.

## Next

- **Backend reshape, shipped as 5.0.0** (see §0 of `docs/openbsd-native-fs/`),
  done in userspace first: node-anchored core, POSIX-free public header, an
  offset-resumable directory iterator, per-edition cache sizing, and no
  `config.h` in the engine.  FUSE keeps working by moving path-split,
  `fill_stat`, and the open-handle table onto the FUSE side.  The installed
  `filsys.h` loses `filsys_readdir` and `filsys_fill_stat`, so this is a major
  bump, not a 4.2.  The OpenBSD read-only V7 driver (Phase 1, §6) is the first
  consumer of 5.0, not parallel work.

- Re-integrate the System III oracle regression test against **prebsd**, where
  the disk images and boot scripts live.  filsys does not ship disk images;
  the oracle fixtures and the test that exercised them moved to prebsd.

## Out of scope

- BSD FFS (4.1cBSD and later) and Minix.
