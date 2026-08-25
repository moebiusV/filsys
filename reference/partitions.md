# RP06 disk partitions (V7 `hp` driver)

The image is a SIMH RP06: 815 cylinders x 19 heads x 22 sectors = 418
blocks/cylinder = 340,670 blocks (174,423,552 bytes).

Partitions are selected by minor device number and their sizes are hardcoded in
the V7 driver (`usr/sys/dev/hp.c`), not stored on disk:

    struct size { int nblocks; int cyloff; } hp_sizes[8] = {
        9614,    0,     /* 0  root   cyl 0..22   */
        8778,   23,     /* 1  swap   cyl 23..43  */
        161348, 44,     /* 2         cyl 44..429 */
        160930, 430,    /* 3         cyl 430..814*/
        153406, 44,     /* 4  RP04/RP05 alias    */
        322278, 44,     /* 5  whole-disk alias   */
    };

Only partitions 0 (root) and 1 (swap) are in use.  The root filesystem was
created by mkfs at 5000 blocks (`fsize=5000`), smaller than the 9614-block
partition 0, leaving the rest of partition 0 spare.

The FUSE driver mounts partition 0 only (blocks 0..4999).
