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
