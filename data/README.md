# Game data

Not in the repository. Quake looks for `QUAKE/id1/pak0.pak` on the disk
(the SD card, or without one the disk image in the SPI flash, see
`../m20/tools/mkdisk/mkdisk.sh`). The shareware `pak0.pak` (18 MB, from
Quake 1.06's shareware release, `quake106.zip`; its `resource.1` unpacks
with 7z) is too big for the flash disk's 6 MB, so for now:

    python3 tools/mkpak.py <path>/ID1/PAK0.PAK data/card/QUAKE/id1/pak0.pak
    ../m20/tools/mkdisk/mkdisk.sh data/card

(5.4 MB: E1M1, the brush models, models, graphics and progs, no sounds
or demos.)

For benchmarks, `timedemo demo1` (the recorded demo played as fast as
possible; it runs on e1m3), on a disk of its own:

    python3 tools/mkpak.py <path>/ID1/PAK0.PAK data/bench/QUAKE/id1/pak0.pak --maps e1m3 --demo demo1
    echo "+timedemo demo1" > data/bench/QUAKE/m20args.txt
    ../m20/tools/mkdisk/mkdisk.sh data/bench

(`QUAKE/m20args.txt`, if it's on the disk, is the command line.)
