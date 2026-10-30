#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Upload a song or the kit's data-flash regions to an FM-1 running RB338, over USB MIDI.

  rb338_upload.py info                 what the device holds
  rb338_upload.py song SONG.rbs        pack a ReBirth song (fm1/pack) and write it to the song slot
  rb338_upload.py song SONG.bin        a song already packed by `fm1pack song`
  rb338_upload.py kit [PREFIX]         kit regions 1 and 2 (default fm1/build/blobs/kit_1.bin, _2.bin);
                                       must come from the same packer run as the firmware's region 0

Protocol: fm1/firmware/src/store.c. Needs mido and python-rtmidi.
"""
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

HDR = [0x7D, 0x46, 0x4C]
INFO, BEGIN, WRITE, END = 0x40, 0x41, 0x42, 0x43
AREAS = {"song": 0, "kit1": 1, "kit2": 2}
CAP = [0x20000 - 16, 0x29000 - 16, 0x16000 - 16]
RC = {0: "ok", 1: "bad argument", 2: "flash error", 3: "CRC mismatch", 4: "no upload open"}
FM1 = Path(__file__).resolve().parent.parent


def pack7(b):
    """groups of up to 7 bytes, each preceded by their top bits"""
    out = []
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << j for j, x in enumerate(g)))
        out += [x & 0x7F for x in g]
    return out


def put7(v, k):
    return [(v >> (7 * i)) & 0x7F for i in range(k)]


def get7(b):
    return sum((x & 0x7F) << (7 * i) for i, x in enumerate(b))


class Link:
    def __init__(self, want=None):
        try:
            import mido
        except ImportError:
            sys.exit("needs mido: pip install mido python-rtmidi")
        self.mido = mido
        keys = [want] if want else ["FM-1", "Felucca", "RB338"]
        pick = lambda names: [n for n in names if any(k in n for k in keys)]
        outs, ins = pick(mido.get_output_names()), pick(mido.get_input_names())
        if not outs or not ins:
            sys.exit(f"no FM-1 MIDI port (looked for {keys}); ports: {mido.get_output_names()}")
        self.o = mido.open_output(outs[0])
        self.i = mido.open_input(ins[0])
        for _ in range(3):                          # the first frame after opening the port can be lost
            try:
                self.req(INFO, [], 1.0)
                break
            except TimeoutError:
                pass

    def req(self, cmd, args, timeout=3.0):
        for _ in self.i.iter_pending():
            pass
        self.o.send(self.mido.Message("sysex", data=HDR + [cmd] + list(args)))
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.i.iter_pending():
                if m.type == "sysex" and list(m.data[:4]) == HDR + [cmd]:
                    return list(m.data[4:])
            time.sleep(0.001)
        raise TimeoutError(f"no reply to command {cmd:#x}")


def info(link):
    r = link.req(INFO, [])
    if not r or r[0] != 1:
        sys.exit(f"unexpected INFO reply {r}: is the FM-1 running RB338?")
    out = {}
    for name, a in AREAS.items():
        q = r[1 + 10 * a: 11 + 10 * a]
        out[name] = (bool(q[0]), get7(q[1:5]), get7(q[5:10]))
    return out


def upload(link, area, data):
    a = AREAS[area]
    if len(data) > CAP[a]:
        sys.exit(f"{area}: {len(data)} B does not fit {CAP[a]} B")
    rc = link.req(BEGIN, [a] + put7(len(data), 4), timeout=15.0)
    if rc[1]:
        sys.exit(f"{area}: erase failed: {RC.get(rc[1], rc[1])}")
    t0 = time.time()
    for off in range(0, len(data), 256):
        for tries in range(3):
            try:
                r = link.req(WRITE, [a] + put7(off, 4) + pack7(data[off:off + 256]))
                break
            except TimeoutError:
                if tries == 2:
                    raise
        if get7(r[1:5]) != off or r[5]:
            sys.exit(f"{area}: write at {off} failed: {RC.get(r[5], r[5])}")
        print(f"\r{area}: {off + min(256, len(data) - off)} / {len(data)} B", end="", flush=True)
    rc = link.req(END, [a] + put7(len(data), 4) + put7(zlib.crc32(data), 5), timeout=10.0)
    print(f"  {time.time() - t0:.1f} s")
    if rc[1]:
        sys.exit(f"{area}: commit failed: {RC.get(rc[1], rc[1])}")


def pack_song(rbs):
    out = Path(tempfile.mkdtemp()) / (rbs.stem + ".bin")
    subprocess.run(["cargo", "run", "-q", "--release", "--", "song", str(rbs), str(out)],
                   cwd=FM1 / "pack", check=True)
    return out.read_bytes()


def main(argv):
    if not argv or argv[0] not in ("info", "song", "kit"):
        sys.exit(__doc__)
    link = Link()
    if argv[0] == "song":
        if len(argv) < 2:
            sys.exit(__doc__)
        p = Path(argv[1])
        data = p.read_bytes() if p.suffix.lower() == ".bin" else pack_song(p)
        if data[:4] != b"ORBS":
            sys.exit(f"{p}: not a packed song")
        upload(link, "song", data)
    elif argv[0] == "kit":
        prefix = argv[1] if len(argv) > 1 else str(FM1 / "build/blobs/kit")
        for r in (1, 2):
            upload(link, f"kit{r}", Path(f"{prefix}_{r}.bin").read_bytes())
    for name, (ok, n, crc) in info(link).items():
        print(f"{name:5} {'ok' if ok else 'empty'}" + (f"  {n} B  crc {crc:08x}" if ok else ""))


if __name__ == "__main__":
    main(sys.argv[1:])
