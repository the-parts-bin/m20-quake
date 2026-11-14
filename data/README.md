# Game data

Not in the repository. Quake looks for `QUAKE/id1/pak0.pak` on the disk:
the SD card, or, without a card or with one that has no `QUAKE/id1`, the
disk image in the SPI flash (`../core/tools/mkdisk/mkdisk.sh`).

The SD card holds the whole shareware `pak0.pak` (18 MB, from Quake 1.06's
shareware release, `quake106.zip`; its `resource.1` unpacks with 7z),
rewritten with every file 32-bit aligned (`tools/alignpak.py`: 64 bytes;
the stock pak has half of them at odd offsets). `../sdcard/make_sdcard.sh`
in m20-system builds the whole card that way; or by hand, written through
the board without taking the card out:

    mkdir -p data/sd/QUAKE/id1 && python3 tools/alignpak.py <path>/ID1/PAK0.PAK data/sd/QUAKE/id1/pak0.pak
    ../core/tools/sdput/sdput.py sync data/sd

(about 4 minutes). Without `QUAKE/m20args.txt` Quake starts as on a PC, with
its demo loop (Esc for the menu; from the flash disk, below, E1M1); for the
benchmark, `make ARGS="+timedemo demo1"` builds that command line in.

The flash disk's 6 MB is too small for it, so there:

    python3 tools/mkpak.py <path>/ID1/PAK0.PAK data/card/QUAKE/id1/pak0.pak
    ../core/tools/mkdisk/mkdisk.sh data/card

(5.4 MB: E1M1, the brush models, models, graphics and progs, no sounds
or demos.)

For benchmarks, `timedemo demo1` (the recorded demo played as fast as
possible; it runs on e1m3), on a disk of its own:

    python3 tools/mkpak.py <path>/ID1/PAK0.PAK data/bench/QUAKE/id1/pak0.pak --maps e1m3 --demo demo1
    echo "+timedemo demo1" > data/bench/QUAKE/m20args.txt
    ../core/tools/mkdisk/mkdisk.sh data/bench

(`QUAKE/m20args.txt`, if it's on the disk, is the command line.)
