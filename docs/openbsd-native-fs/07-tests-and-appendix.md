# 8. Test strategy

- **Unchanged userspace suite is the safety net.** `make check` and `test.sh`
  must pass byte-for-byte after any engine change; the kernel driver must not
  alter userspace behavior at all.
- **Transport parity first.** Before booting a kernel, run `test_matrix`
  against a `filsys_io_kern`-shaped transport in a userspace harness (the
  `filsys_set_io` hook already exists for exactly this). If the codecs pass
  over the new transport, the remaining risk is confined to the vnode/vfs glue,
  not the format logic.
- **vnd-backed mount in a VM.** `vnconfig(8)` a V7 image, `mount_filsys`, run
  the FUSE `test.sh` read/write/rename/truncate/persistence sequence against the
  native mount; then unmount and `fsck.filsys` the image.
- **Oracle diff.** Mount each edition read-only and diff a recursive copy
  against the userspace `mount.filsys` result for the same image.
- **Concurrency smoke.** Parallel `find`/`grep` (readers) and a writer loop,
  checking for corruption under the big lock, then `fsck` clean.

---

# Appendix A. Reference material

## A.1 The five registration diffs (filsys edition, exact shape)

```c
/* sys/conf/files */
file filsys/filsys_vfsops.c	filsys
file filsys/filsys_vnops.c	filsys
file filsys/filsys_io_kern.c	filsys
file filsys/filsys_vnode.h		# not compiled; header
# + the libfilsys engine sources, each gated on `filsys`

/* sys/conf/GENERIC */
option		FILSYS		# Research Unix (V1..V10, 2.11BSD, SysIII/V) filesystems

/* sys/kern/vfs_init.c */
{ &filsys_vfsops, MOUNT_FILSYS, 20, 0, MNT_LOCAL, sizeof(struct filsys_args) },

/* sys/sys/mount.h */
struct filsys_args {
    char   *fspec;          /* block special device to mount */
    int     edition;        /* FILSYS_* selector */
    uint64_t offset;        /* byte offset of the fs within the device */
    int     readonly;
    char   *arch;           /* "vax"/"x86"/"3b2"/"68k", or NULL */
};
#define MOUNT_FILSYS	"filsys"
extern const struct vfsops filsys_vfsops;

/* sys/sys/vnode.h */
enum vtagtype { ..., VT_FILSYS, };
```

## A.2 The `struct vfsops` / `struct vops` field lists (current OpenBSD)

`vfsops`: `vfs_mount vfs_start vfs_unmount vfs_root vfs_quotactl vfs_statfs
vfs_sync vfs_vget vfs_fhtovp vfs_vptofh vfs_init vfs_sysctl vfs_checkexp`.

`vops`: `vop_lookup vop_create vop_mknod vop_open vop_close vop_access
vop_getattr vop_setattr vop_read vop_write vop_ioctl vop_kqfilter vop_revoke
vop_fsync vop_remove vop_link vop_rename vop_mkdir vop_rmdir vop_symlink
vop_readdir vop_readlink vop_abortop vop_inactive vop_reclaim vop_lock
vop_unlock vop_bmap vop_strategy vop_print vop_islocked vop_pathconf
vop_advlock vop_bwrite`.

## A.3 libfilsys engine libc-dependency inventory (as-is kernel mapping)

| symbol | count (engine) | kernel mapping |
|---|---|---|
| `free` | 45 | `filsys_free` (§0) |
| `printf` / `fprintf` | 19 | kernel `printf` (or drop in runtime subset) |
| `strcmp`/`strncmp`/`memcmp` | 31 | libkern |
| `open` / `close` | 4 / 20 | excluded — the codecs' own open/close path is not compiled in; the driver opens the device (§4.1) |
| `memset`/`memcpy`/`memmove` | 53 | libkern |
| `snprintf`/`vsnprintf` | 15 | kernel `snprintf` |
| `calloc`/`malloc`/`realloc` | 18 | `filsys_alloc` (§0) |
| `strlen`/`strcpy`/`strlcpy` | 11 | libkern |
| `fcntl` | 2 | dropped (userspace advisory-lock path only) |
| `pread`/`pwrite` | 3 | `filsys_io_kern` (the default `filsys_io_file` is excluded) |
| `stat` | 0 | — dropped (driver maps inode → `vattr`) |
| `fsync` | 2 | dropped (driver's `vfs_sync`/`VOP_FSYNC` handle flush) |
| `getchar`/`feof`/`fflush` | (check.c) | **excluded** — interactive fsck stays userspace |

`errno` values used by the engine (`EIO`, `EINVAL`, `EROFS`, `ENOSPC`, `ENOENT`,
`ENOTDIR`, `EEXIST`, …) are the same constants in the kernel.

## A.4 References

- GEFS on OpenBSD announcement — https://marc.info/?l=openbsd-tech&m=178948744271633&w=2
- GEFS paper — https://orib.dev/gefs.pdf  (patch: https://orib.dev/gefs.diff)
- GEFS repo — `git://shithub.us/ori/openbsd`, branch `gefs`
- Phoronix coverage — https://www.phoronix.com/news/OpenBSD-GEFS-File-System
- filsys home — https://github.com/moebiusV/filsys
- OpenBSD source — https://github.com/openbsd/src
