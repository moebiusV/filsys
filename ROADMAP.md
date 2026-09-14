# Roadmap

filsys is at 4.1.1.  Version numbers follow [Semantic Versioning](https://semver.org/)
per [VERSIONING.md](VERSIONING.md).

## Shipped

- Research Unix filesystems from PDP-7 (V0) through V10, plus 32V, Coherent,
  Xenix, 2.9BSD, 2.11BSD, System III, and System V (s5fs) — `mount`, `fsck`,
  `mkfs`, and `findfs`.

## Next

- Re-integrate the System III oracle regression test against **prebsd**, where
  the disk images and boot scripts live.  filsys does not ship disk images;
  the oracle fixtures and the test that exercised them moved to prebsd.

## Out of scope

- BSD FFS (4.1cBSD and later) and Minix.
