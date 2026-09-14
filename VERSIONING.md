# Versioning

filsys follows [Semantic Versioning](https://semver.org/) 2.0.0.

## The public contract

The public API is the installed `filsys.h` header.  The on-disk filesystem
formats are defined by the Research Unix / BSD / System V specifications and are
therefore stable by contract; a change to how an existing image is decoded is a
breaking change.  Internal headers (`filsys_ops.h`, `v7fs.h`, and the per-backend
headers) are not part of the contract and may change without a version bump.

- **MAJOR (x)** — any backward-incompatible change: a change to a `filsys.h`
  symbol's signature or semantics, a removed symbol, a change to a `filsys.h`
  struct's layout, or a change to how an existing on-disk format is decoded.
- **MINOR (y)** — backward-compatible additions: a new function, a new enum
  value, a new field on a `filsys.h` struct.
- **PATCH (z)** — backward-compatible bug fixes only.

## Version history

Releases before this policy were numbered under a looser scheme.  They were
re-numbered in 2026-09 to this mapping (the three releases marked with a
backward-incompatible change are the ones that should have bumped MAJOR):

| old       | current | reason |
|-----------|---------|--------|
| 1.2.3     | 1.2.3   | baseline |
| 1.2.4     | 1.2.4   | bug fix |
| 1.2.5     | 1.2.5   | bug fix |
| 1.2.6     | 1.2.6   | bug fix |
| 1.2.7     | 1.3.0   | + Coherent edition |
| 1.3.0     | 2.0.0   | **`filsys_dirent_t.name` widened 15→64** + Xenix/2.9BSD/2.11BSD |
| 1.3.1     | 2.0.1   | bug fix |
| 1.3.2     | 2.0.2   | bug fix |
| 1.4.0     | 2.0.3   | refactor |
| 1.5.0     | 2.0.4   | refactor |
| 1.5.1     | 2.0.5   | cleanup |
| 1.5.2     | 2.1.0   | + edition-name table API |
| 1.6.0     | 2.1.1   | refactor |
| 1.7.0     | 2.1.2   | refactor |
| 1.8.0     | 2.1.3   | PDP-7 word codecs |
| 1.9.0     | 3.0.0   | **`filsys_close` return type `void`→`int`** + V8/V9/V10/SysIII/SVR2/SVR4 |
| 1.9.1     | 3.0.1   | bug fix |
| 1.9.2     | 3.0.2   | bug fix |
| 1.9.3     | 3.1.0   | + `filsys_stat_ino` / `filsys_truncate_ino` |
| 1.9.4     | 3.1.1   | bug fix |
| 2.0.0     | 3.2.0   | + `filsys_detect` / `FILSYS_UNIX` |
| 2.1.0     | 3.2.1   | docs/test only |
| 2.2.0     | 4.0.0   | **`filsys_create` gained `uint32_t *ino`** |
| 2.3.0     | 4.1.0   | + probe-confidence enum / `filsys_detect_t.conf` |

Versions 1.0.0–1.2.2 predate the current git history (tarballs only) and are
not re-tagged.
