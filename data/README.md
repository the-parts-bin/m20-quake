# Game data

Not in the repository. Quake looks for `QUAKE/id1/pak0.pak` on the disk
(the SD card, or without one the disk image in the SPI flash, see
`../m20/tools/mkdisk/mkdisk.sh`). The shareware `pak0.pak` (18 MB, from
Quake 1.06's shareware release) is too big for the flash disk's 6 MB; a
trimmed one is planned.
