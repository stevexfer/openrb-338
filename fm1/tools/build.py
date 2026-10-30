#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Build Felucca: the app, the update loader and an installable .fwsc package.

  tools/build.py [--release X.Y[-suffix]]

Outputs in build/: felucca.bin (app), loader/ota.bin (update loader),
felucca.fwsc (package). See BUILDING.md for the toolchain and the SDK.

The JieLi toolchain is Linux x86-64 only. JIELI_TOOLCHAIN points at it; on
macOS (or with JIELI_DOCKER=1) each tool runs in a linux/amd64 container.
"""
import argparse
import hashlib
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
FW = SRC / "firmware"
OUT = SRC / "build"
GEN = OUT / "gen"
LDR = OUT / "loader"
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402

APP_XIP = 0x02000120                # app.bin offset 0 in the XIP map; the SPL jumps here
APP_SLOT = fm1pkg_make.APP_SLOT
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"    # the file name the SPL looks for
DOCKER_IMAGE = os.environ.get("JIELI_DOCKER_IMAGE", "debian:bookworm-slim")
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
# The engine (engine/*.cc) and its glue (firmware/src/*.cc): the AC79 core with its single-precision FPU, as
# MOOD builds it. Floats live in the general registers, so the C side calls it through int / pointer
# arguments only. No exceptions, RTTI or static constructors (nothing runs .init_array). The engine's
# `real` is float here; the sequencer keeps its doubles (soft-double, once per 64-frame block).
CXXFLAGS = ["-mcpu=r3", "-mfprev1", "-O2", "-std=gnu++11", "-fno-math-errno", "-fno-exceptions", "-fno-rtti",
            "-fno-threadsafe-statics", "-fno-use-cxa-atexit", "-nostdinc++", "-ffunction-sections",
            "-fdata-sections", "-ffp-contract=off", "-Wall", "-Wno-unused-function", "-Wframe-larger-than=1024",
            "-Wdouble-promotion", "-Werror=double-promotion", "-DORB_REAL=float", "-DORB_DELAY_F16",
            "-DORB_POOL_NODES=2048", "-DORB_MAX_FRAMES=128", "-DORB_MERGE_CHUNK=256", "-Iengine"]
CXX_LIBS = ["libm.a", "libcompiler_rt.a"]     # the toolchain's FPU (r3) builds
ENGINE = SRC / "engine"
BLOBS = OUT / "blobs"
KIT0_CAP = 400_000                  # kit region 0, in the app image (pack/src/main.rs FM1_REGIONS)
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")

# SDK files of AC79NN_SDK_V1.2.1_2023-12-13 (the tested version)
SDK_SHA256 = {
    "uboot.boot": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
    "cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "cfg/eq_cfg_hw.bin": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d",
}

PRODUCT = "FM-1_338"                # FM1-RB338 Felucca identity
VERSION = None
APP = "rb338"


def toolchain():
    tc = os.environ.get("JIELI_TOOLCHAIN")
    if not tc or not (Path(tc) / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit("JIELI_TOOLCHAIN must point at the JieLi Linux toolchain "
                         "(the directory with pi32v2/ and common/; see tools/get_toolchain.sh)")
    return Path(tc).resolve()


def use_docker():
    native = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")
    return os.environ.get("JIELI_DOCKER", "0" if native else "1") == "1"


def sdk_root():
    p = os.environ.get("AC79_SDK")
    root = Path(p).resolve() if p else (Path.home() / "fw-AC79_AIoT_SDK").resolve()
    if not (root / "include_lib" / "newlib").is_dir():
        raise SystemExit(f"AC79_SDK must point at the AC79 SDK checkout (missing include_lib/newlib in {root})")
    return root


def sdk_paths():
    """Include/lib paths for the host or for the linux/amd64 Docker build."""
    root = sdk_root()
    if use_docker():
        p = Path("/opt/ac79")
        return {
            "newlib_inc": p / "include_lib" / "newlib" / "include",
            "cpp_inc": p / "include_lib" / "c++" / "include",
            "cpp_pthread": p / "include_lib" / "c++" / "simple_pthread",
            "system": p / "include_lib" / "system",
            "system_os": p / "include_lib" / "system" / "os",
            "cpp_lib": p / "include_lib" / "c++" / "libstdc++",
            "newlib_lib": p / "include_lib" / "newlib" / "pi32v2-lib",
        }
    return {
        "newlib_inc": root / "include_lib" / "newlib" / "include",
        "cpp_inc": root / "include_lib" / "c++" / "include",
        "cpp_pthread": root / "include_lib" / "c++" / "simple_pthread",
        "system": root / "include_lib" / "system",
        "system_os": root / "include_lib" / "system" / "os",
        "cpp_lib": root / "include_lib" / "c++" / "libstdc++",
        "newlib_lib": root / "include_lib" / "newlib" / "pi32v2-lib",
    }


def tc_path(rel):
    """a path inside the toolchain, as the tool sees it (in the container: /opt/jieli)"""
    return f"/opt/jieli/{rel}" if use_docker() else str(toolchain() / rel)


def tc(tool, *args):
    """run a toolchain binary (pi32v2/bin/..., common/bin/...) with cwd SRC; paths relative to SRC when under SRC"""
    def tc_arg(a):
        if not isinstance(a, Path):
            return a
        p = a.resolve()
        try:
            return str(p.relative_to(SRC))
        except ValueError:
            return str(p)

    rel = [tc_arg(a) if isinstance(a, Path) else a for a in args]
    if tool == "cc":                # the toolchain's cc wrapper needs python3; call clang directly
        tool, rel = "pi32v2/bin/clang", ["-target", "pi32v2", *rel]
    elif tool == "cxx":
        tool, rel = "pi32v2/bin/clang++", ["-target", "pi32v2", "-isystem", tc_path("pi32v2/include"), *rel]
    if use_docker():
        cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SRC}:/work",
               "-v", f"{toolchain()}:/opt/jieli:ro", "-v", f"{sdk_root()}:/opt/ac79:ro",
               "-w", "/work", DOCKER_IMAGE, f"/opt/jieli/{tool}", *rel]
    else:
        cmd = [str(toolchain() / tool), *rel]
    r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"build: {tool} failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return r.stdout


def tc_all(*cmds):
    with ThreadPoolExecutor(len(cmds)) as ex:
        return list(ex.map(lambda c: tc(*c), cmds))


# ---- update loader

def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
    """ota.bin = outer JLFS head (32 B) + inner head (32 B, load address) +
    blocks [clen u32][dlen u32][LZ4 block], dlen 4096 except the last"""
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def build_loader():
    LDR.mkdir(parents=True, exist_ok=True)
    src = FW / "loader"
    flags = [*CFLAGS, "-Ifirmware/hal", "-Ifirmware/src"]
    tc_all(("cc", "-c", src / "crt0_ldr.S", "-o", LDR / "crt0_ldr.o"),
           ("cc", *flags, "-c", src / "loader.c", "-o", LDR / "loader.o"))
    elf = LDR / "loader.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld",
       LDR / "crt0_ldr.o", LDR / "loader.o", "-o", elf)
    _, dis, hdr, syms = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, LDR / "loader.bin"),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-h", elf),
                               ("common/bin/objdump", "-t", elf))
    (LDR / "loader.dis").write_text(dis)
    for ln in hdr.splitlines():     # the loader rewrites the flash: nothing may run from XIP
        p = ln.split()
        if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
            raise SystemExit(f"loader: section {p[1]} at {p[3]} is outside RAM")
    image = (LDR / "loader.bin").read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    ota = ldr_wrap(image)
    (LDR / "ota.bin").write_bytes(ota)
    bss = [int(ln.split()[0], 16) for ln in syms.splitlines() if ln.rstrip().endswith("_bss_end")]
    print(f"loader: image {len(image)} B at {LOADER_LOAD:#x}" + (f", bss end {bss[0]:#x}" if bss else ""))
    print(f"loader: ota.bin {len(ota)} B")
    return ota


# ---- app


def ensure_blobs():
    """the tables blob and kit region 0 linked into the image (and kit regions 1-2 for rb338_upload.py),
    made by fm1/pack from the upstream assets when missing"""
    need = [BLOBS / "tables.bin", BLOBS / "kit_0.bin", BLOBS / "kit_1.bin", BLOBS / "kit_2.bin", BLOBS / DEMO_SONG]
    if all(p.exists() for p in need):
        return
    cargo = shutil.which("cargo") or str(Path.home() / ".cargo" / "bin" / "cargo")
    if not Path(cargo).exists():
        raise SystemExit("build: the sample kit needs Rust (cargo) and the flac CLI to make build/blobs once")
    BLOBS.mkdir(parents=True, exist_ok=True)
    demo = SRC.parent / "tests" / "songs" / "shipped" / "ReBirth 1.0 Default Song.rbs"
    for args in (["tables", BLOBS / "tables.bin"], ["kit", BLOBS / "kit"], ["song", demo, BLOBS / DEMO_SONG]):
        subprocess.run([cargo, "run", "-q", "--release", "--", *map(str, args)], cwd=SRC / "pack", check=True)


def blobs_asm():
    """build/gen/blobs.S: the blobs as .rodata in XIP, word aligned"""
    gen = OUT / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    s = []
    for name, f in (("orb_tables", "tables.bin"), ("orb_kit0", "kit_0.bin")):
        s += ['    .section .rodata.' + name + ',"a"', "    .balign 4", f"    .global {name}, {name}_end",
              f"{name}:", f'    .incbin "build/blobs/{f}"', f"{name}_end:"]
    (gen / "blobs.S").write_text("\n".join(s) + "\n")
    return gen / "blobs.S"


def build_cxx():
    objdir = OUT / "cxx"
    objdir.mkdir(parents=True, exist_ok=True)
    srcs = [*sorted(ENGINE.glob("*.cc")), *sorted((FW / "src").glob("*.cc"))]
    objs = [objdir / (s.stem + ".o") for s in srcs]
    tc_all(*[("cxx", *CXXFLAGS, "-Ifirmware/src", "-c", s, "-o", o) for s, o in zip(srcs, objs)])
    return objs


def build_app():
    sp = sdk_paths()
    inc = ["-Ifirmware/hal", "-Ifirmware/src", "-Ifirmware/gen"]
    flags = [*CFLAGS, *inc, f'-DFELUCCA_ID="{PRODUCT}"', "-DFM1_DEVICE"]
    cflags_libc = ["-Os", "-ffunction-sections", "-fdata-sections", "-w", "-Ifirmware/hal",
                   f"-I{sp['newlib_inc']}", "-DFM1_DEVICE"]
    ld = FW / f"{APP}.ld"
    main_o = OUT / f"{APP}.o"
    elf = OUT / f"{APP}.elf"
    cmds = [("cc", "-c", FW / "crt0.S", "-o", OUT / "crt0.o"),
            ("cc", "-c", FW / "hal" / "fm1_vec.S", "-o", OUT / "fm1_vec.o"),
            ("cc", "-c", FW / "hal" / "fm1_isr.S", "-o", OUT / "fm1_isr.o"),
            ("cc", *flags, "-c", FW / "src" / f"{APP}_main.c", "-o", main_o),
            ("cc", *cflags_libc, "-c", FW / "src" / "fm1_libc.c", "-o", OUT / "fm1_libc.o")]
    ensure_blobs()
    kit0 = (BLOBS / "kit_0.bin").stat().st_size
    if kit0 > KIT0_CAP:
        raise SystemExit(f"build: kit region 0 is {kit0} B, over {KIT0_CAP}")
    cmds.append(("cc", "-c", blobs_asm(), "-o", OUT / "blobs.o"))
    tc_all(*cmds)
    cxx_objs = build_cxx()
    cxx_libs = [tc_path(f"pi32v2/lib/r3/{lib}") for lib in CXX_LIBS]
    c_libs = [sp["newlib_lib"] / f for f in ("libc.a", "libm.a", "libcompiler_rt.a", "libc.a")]
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", ld, OUT / "crt0.o", OUT / "fm1_vec.o",
       OUT / "fm1_isr.o", main_o, OUT / "fm1_libc.o", OUT / "blobs.o", *cxx_objs,
       "--start-group", *cxx_libs, "--end-group", *c_libs, "-o", elf)
    for sect in ("text.bin", "data.bin", "ramtext.bin"):
        (OUT / sect).unlink(missing_ok=True)
    *_, syms, dis, rt = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, OUT / "text.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".data", elf, OUT / "data.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".ram_text", elf, OUT / "ramtext.bin"),
                               ("common/bin/objdump", "-t", elf),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-d", "-j", ".ram_text", elf))
    (OUT / f"{APP}.dis").write_text(dis)
    (OUT / f"{APP}.sym").write_text(syms)

    def symv(name):
        return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)
    img = bytearray((OUT / "text.bin").read_bytes())
    # .ram_text and .data follow .text at their load addresses; crt0 copies them by words
    for sect, lname in (("ramtext.bin", "_rt_load"), ("data.bin", "_data_load")):
        load = symv(lname)
        if load % 4:
            raise SystemExit(f"{lname} {load:#x} is not word aligned")
        blob = (OUT / sect).read_bytes() if (OUT / sect).exists() else b""
        if blob:
            if load - APP_XIP < len(img):
                raise SystemExit(f"{lname} overlaps the image")
            img += b"\xff" * (load - APP_XIP - len(img))
            img += blob
    img += b"\xff" * (-len(img) % 4)
    (OUT / f"{APP}.bin").write_bytes(img)
    return bytes(img), syms, dis, rt


def check(img, syms, dis, rt):
    errors, notes = [], []
    m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
    if not m or int(m.group(1), 16) != APP_XIP:
        errors.append(f"_start is not at {APP_XIP:#x}")
    if img[:4] != bytes.fromhex("04818000"):
        errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
    rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
    if rt_calls:                    # RAM code runs with the flash off: no calls into XIP
        errors.append(f".ram_text contains calls: {rt_calls[:3]}")
    else:
        notes.append(f".ram_text: {len([ln for ln in rt.splitlines() if LINE.match(ln)])} insns, no calls")
    for ln in dis.splitlines():     # nothing may call or load an address in the chip ROM
        mm = LINE.match(ln)
        if not mm:
            continue
        if "call " in mm.group(3):
            for call_m in re.findall(r"call -?\d+ <[^:>]*: ([0-9a-f]+) >", mm.group(3)):
                val = int(call_m, 16) & 0xFFFFFFFF
                if 0xFFC00000 <= val < 0xFFD00000:
                    errors.append(f"call into chip ROM at {val:#010x}")
    if len(img) > APP_SLOT:
        errors.append(f"image {len(img)} B exceeds the app slot")
    # the engine's floats run on the FPU: a soft single-precision routine means a unit missed -mfprev1 or
    # an operation the FPU lacks (float <-> int64); soft doubles are expected only where the engine keeps
    # f64 on purpose (the sequencer's clock, table setup), so their callers are listed
    import bisect
    funcs = sorted((int(m.group(1), 16), m.group(2)) for m in
                   re.finditer(r"^([0-9a-f]+) .*\.text\s+[0-9a-f]+ (\S+)$", syms, re.M))
    starts = [a for a, _ in funcs]
    soft_callers = {}
    soft = re.compile(r"call [^<]*<(__(?:add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge|un|extend|trunc|fix|fixuns|float"
                      r"|floatun)\w*[sd]f\w*)")
    for ln in dis.splitlines():
        mm = LINE.match(ln)
        if not mm:
            continue
        for r in soft.findall(mm.group(3)):
            i = bisect.bisect_right(starts, int(mm.group(1), 16)) - 1
            soft_callers.setdefault(r, set()).add(funcs[i][1] if i >= 0 else "?")
    single = sorted(r for r in soft_callers if "df" not in r)
    if single:
        errors.append("soft single-precision routines called: " +
                      "; ".join(f"{r} from {sorted(soft_callers[r])[:4]}" for r in single))
    dbl = sorted({f for r, fs in soft_callers.items() if "df" in r for f in fs})
    if dbl:
        notes.append(f"soft-double callers ({len(dbl)}): {', '.join(dbl[:12])}{' ...' if len(dbl) > 12 else ''}")

    def sym(name):
        mm = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
        return int(mm.group(1), 16) if mm else 0
    used = sym("_pool_end") - 0x01C08000
    heap = sym("_heap_end") - sym("_heap_start")
    notes.append(f"image {len(img)} B of {APP_SLOT}; RAM .data+.bss {used} B; heap {heap} B")
    return errors, notes


# Every hardware register access lives in hal/. In src/ and loader/ (comments stripped):
#  - no volatile pointer cast, except of a C object's address (`*(volatile T *)&x`: a read-once
#    of a RAM flag shared with an ISR, e.g. usb.c ota_wire_send);
#  - no literal in a register or reserved window (core SFRs 0x10000-0x13FFF, SFC 0x40000-0x43FFF,
#    GPIO/IOMAP 0x50000-0x51FFF, CPU 0x1EE0000-0x1EEFFFF, RAM top 0x01C7F000- (boot info,
#    mailbox, vectors), XIP 0x02000000-0x020FFFFF);
#  - no inline asm, except the empty compiler barrier RING_PUBLISH() (emits no instruction);
#  - no HAL register macro (a hal/ #define that is, or expands to, a volatile access).
# Linker symbols (_bss_start[], _rt_load[], ...) are plain C objects and pass.
MMIO_LIT = re.compile(r"\b0x0*(1[0-3][0-9a-f]{3}|4[0-3][0-9a-f]{3}|5[01][0-9a-f]{3}|1ee[0-9a-f]{4}|"
                      r"1c7f[0-9a-f]{3}|20[0-9a-f]{5})u?l?\b", re.I)
MMIO_CAST = re.compile(r"\(\s*(?:const\s+)?volatile\b[^()]*\*\s*\)(?!\s*&)")
MMIO_ASM = re.compile(r"\b(?:__asm__|asm)\b(?!\s+volatile\s*\(\s*\"\"\s*:::\s*\"memory\"\s*\))")


def mmio_check():
    def strip(s):
        s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
        return re.sub(r"//[^\n]*", "", s)
    defs = {}
    for h in sorted((FW / "hal").glob("*.h")):
        for m in re.finditer(r"^#define\s+(\w+)(?:\([^)]*\))?\s+(.*)$", strip(h.read_text()), re.M):
            defs[m.group(1)] = m.group(2)
    regs = {n for n, b in defs.items() if re.search(r"\bvolatile\b", b)}
    while True:                                   # macros built on register macros (FM1_WR_LIMIT_H -> FM1_X2)
        more = {n for n, b in defs.items() if n not in regs and set(re.findall(r"\w+", b)) & regs}
        if not more:
            break
        regs |= more
    errors = []
    for f in sorted([*(FW / "src").glob("*.[ch]"), *(FW / "loader").glob("*.c")]):
        for no, ln in enumerate(strip(f.read_text()).splitlines(), 1):
            where = f"{f.relative_to(FW)}:{no}"
            for rx, what in ((MMIO_LIT, "register/window address"), (MMIO_CAST, "volatile pointer cast"),
                             (MMIO_ASM, "inline asm")):
                m = rx.search(ln)
                if m:
                    errors.append(f"{where}: {what} outside hal/ ({m.group(0).strip()}); add a hal/ helper")
            used = set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", ln)) & regs
            if used:
                errors.append(f"{where}: HAL register macro {sorted(used)[0]} outside hal/; use a hal/ helper")
    return errors


def main():
    global PRODUCT, VERSION
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", metavar="X.Y", help="release build: identity FM-1_9XY, version string X.Y")
    ap.add_argument("--sdk", type=Path, help="JieLi AC79 SDK checkout (default: $AC79_SDK)")
    a = ap.parse_args()
    name = f"{APP}.fwsc"
    if a.release:
        m = re.fullmatch(r"(\d+)\.(\d+)(-[A-Za-z0-9]+)?", a.release)
        if not m:
            raise SystemExit(f"--release {a.release}: use X.Y or X.Y-suffix")
        VERSION = a.release
        name = f"{APP}-{a.release}.fwsc"
    fm1pkg_make.SDK = a.sdk
    for rel, sha in SDK_SHA256.items():          # fail early without the SDK
        if hashlib.sha256(fm1pkg_make.sdk_file(rel)).hexdigest() != sha:
            print(f"warning: SDK {rel} differs from AC79NN_SDK_V1.2.1; the package will not match the reference")
    OUT.mkdir(parents=True, exist_ok=True)
    ota = build_loader()
    img, syms, dis, rt = build_app()
    errors, notes = check(img, syms, dis, rt)
    hal_err = mmio_check()
    errors += hal_err
    if not hal_err:
        notes.append("register access: hal/ only (src/, loader/ clean)")
    for n in notes:
        print("  ok   ", n)
    for e in errors:
        print("  FAIL ", e)
    if errors:
        raise SystemExit("build: checks failed")
    pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, PRODUCT)
    (OUT / name).write_bytes(pkg)
    print(f"app      {OUT / f'{APP}.bin'}  {len(img)} B")
    print(f"loader   {LDR / 'ota.bin'}  {len(ota)} B")
    print(f"package  {OUT / name}  {len(pkg)} B, identity {PRODUCT}")
    stem = Path(name).stem
    save, demo = OUT / f"{stem}.fm1save", OUT / f"{stem}_demo.fm1save"
    save.write_bytes(fm1save(pkg, stem, STORE_AREAS))
    demo.write_bytes(fm1save(pkg, f"{stem} demo", {**STORE_AREAS, DEMO_SONG: SONG_AREA}))
    print(f"emulator {save}  (the package and kit regions 1-2 in flash, for fm1-emu / FM1Ception)")
    print(f"emulator {demo}  (the same with the ReBirth 1.0 Default Song in the song slot)")
    return 0


# Data-flash areas (firmware/src/store.c): a 16-byte header {"RBA1", len, crc32, 0}, then the data
STORE_AREAS = {"kit_1.bin": 0xB7000, "kit_2.bin": 0xE5000}
SONG_AREA = 0x97000
DEMO_SONG = "demo_song.bin"


def fm1save(pkg, name, areas):
    """an fm1-emu v1 save (crates/fm1-plugin/src/savefile.rs): the package and the flash sectors of
    the given data-flash areas, as rb338_upload.py would write them on a real FM-1"""
    import base64
    import json
    import zlib
    sectors = []
    for f, at in areas.items():
        d = (BLOBS / f).read_bytes()
        area = b"RBA1" + struct.pack("<III", len(d), zlib.crc32(d), 0) + d
        area += b"\xff" * (-len(area) % 4096)
        for o in range(0, len(area), 4096):
            sectors.append({"at": at + o, "data": base64.b64encode(area[o:o + 4096]).decode()})
    return json.dumps({"format": "fm1-emulator-save", "version": 1, "hash": hashlib.sha256(pkg).hexdigest(),
                       "name": name, "package": base64.b64encode(pkg).decode(), "sectors": sectors}).encode()


if __name__ == "__main__":
    sys.exit(main())
