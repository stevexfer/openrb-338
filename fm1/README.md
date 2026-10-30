# FM1-RB338: RB-338 firmware for the M-VAVE FM-1

OpenRB-338's engine ported to C++ and running as firmware on the M-VAVE FM-1 (JieLi AC79),
packaged as a Felucca `.fwsc` update with the identity `FM-1_338`. The HAL (`firmware/hal`), the
update loader and the packaging tools (`tools/build.py`, `fm1pkg_make.py`, `lz4blk.py`) come from
Hügelton Instruments' Felucca (GPL-3.0-only), as their headers say; OpenRB-338 is GPL-3.0.

## Build

Needs the JieLi pi32v2 toolchain (`JIELI_TOOLCHAIN`, default `~/.jieli/toolchain`), the AC79 SDK
(`AC79_SDK`, default `~/fw-AC79_AIoT_SDK`), and Docker on macOS.

```sh
cd fm1
./build.sh
# -> build/rb338.fwsc            (identity FM-1_338)
#    build/rb338.fm1save         for the fm1-emu emulator: the firmware with kit regions 1-2 in flash, no song
#    build/rb338_demo.fm1save    the same with the ReBirth 1.0 Default Song in the song slot
```

The first build also runs `pack/` (Rust, plus the `flac` CLI) once to make `build/blobs/`: the
engine's tables, the sample kit and the demo song, all from the upstream assets.

The C++ engine (`engine/`, a port of `orb-engine`) plays the song in the song slot from the audio
ISR, with the front panel below (`firmware/src/ui.c`). Songs and the kit are uploaded over USB
(`tools/rb338_upload.py`).

## Front panel

The screen is the RB-338 rack in ReBirth's colours, drawn in code (no bitmaps), one module a page:
HOME (the rack: tempo, transport, the four machines' patterns and steps, meters, effects), TB-303
#1 and #2, TR-808, TR-909, MIXER, EFFECTS (PCF, delay, distortion, compressor).

| Control | Does |
| --- | --- |
| SELECT | previous / next page; HOME, GLO (mixer), FX jump there |
| K1..K4 | the four knobs of the bank shown in the footer; ALGORITHM picks the bank |
| PRESETS | the machine's pattern (A1..D8, switched at the bar) |
| PLAY / SEQ | start / stop; pattern / song mode |
| White keys | machine pages: 1..8 the pattern in the bank, 9..12 the bank; HOME and MIXER: 1..4 mute the channels; EFFECTS: 1..4 power PCF, delay, distortion, compressor |
| Black keys | 808 / 909: pick the instrument (OCT-/OCT+ too, with the accent) |
| EDIT | step edit on a machine page (the EDIT LED) |
| SAVE, SAVE | writes the song back to the song slot (survives power-off; a new song when the slot was empty) |

Step edit, 808 / 909: the white keys toggle the instrument's 16 steps (909: strong, weak, off).
303: a key writes its note at the cursor (C4 is C; below F3..B3 the octave down, above C5 up) and
moves on; OCT-/OCT+ move the cursor, ENV toggles the accent, LFO the slide, SCL the rest, REC
writes a rest and moves on. Pattern length and shuffle are bank 3 of the 303s, bank 2 of the drums.
In song mode the song's automation keeps setting the knobs it records.

In fm1-emu the shipped songs average 10–20% of the CPU, the worst 2.9 ms render half 70%
(`FM1_LIVE=.../fm1-live tools/emu_cpu.sh [SECONDS] [SONG ...]`). Against the reference
renders the FM-1 build scores 52–62 dB SNR on the busy songs, Mutilated Species II 45 dB
(`engine/test/run.sh`); with the uncompressed kit 65–88 dB (Mutilated Species II 50 dB), so the
kit's rounding is what limits it.

## Song and kit upload

```sh
/usr/bin/python3 -m venv .venv                    # macOS's Python 3.9: python-rtmidi has wheels for it
.venv/bin/pip install mido python-rtmidi
.venv/bin/python tools/rb338_upload.py kit                # once per firmware build: kit regions 1 and 2
.venv/bin/python tools/rb338_upload.py song MySong.rbs    # packs the song with pack/, writes the song slot
.venv/bin/python tools/rb338_upload.py info
```

## Flash map

| Range | Use |
|---|---|
| 0x004000–0x093000 | app area (OTA package): code, engine tables, kit region 0 (400000 B) |
| 0x093000–0x097000 | left alone (the update loader's record scan) |
| 0x097000–0x0B7000 | song slot (128 KiB) |
| 0x0B7000–0x0E0000 | kit region 1 (164 KiB) |
| 0x0E0000–0x0E5000 | OTA staging, update record at 0xE4F00 |
| 0x0E5000–0x0FB000 | kit region 2 (88 KiB) |
| 0x0FB000–0x0FC000 | settings |
| 0x0FC000–          | Felucca globals, system: not written |

Each uploaded area starts with a 16-byte header (`RBA1`, length, CRC-32) written last, so an
interrupted upload leaves the area empty. The kit's 45 drum samples are FLAC (blocksize 256). To fit, the 39
largest are rounded so the error stays at least 51 dB below the signal in every 256-sample frame
(52.4 dB or better over each whole sample); the rest are lossless.

## Host engine (reference)

From the repo root:

```sh
cargo build --release -p orb-cli
# ./target/release/orb render ...
```

`engine/test/run.sh` checks the C++ engine (double build) against `orb render`, sample for sample.
