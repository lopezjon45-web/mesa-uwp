#!/usr/bin/env python3
"""Runs every glprobe scene in its own process and compares each image with its reference.

    run.py --exe glprobe.exe --out DIR --expect-all                 every scene must pass (DXIL path)
    run.py --exe glprobe.exe --out DIR --expect expected.txt        only the listed scenes must pass (ratchet)
    run.py --exe glprobe.exe --out DIR --write-ref ref/             also copy each rendered image into ref/

A scene passes when glprobe exits 0, GL_RENDERER contains --renderer, and every channel of every pixel is
within TOLERANCE of the reference. Each scene runs in its own process so a driver crash fails only that
scene. The per-scene table is printed (and appended to $GITHUB_STEP_SUMMARY) whatever the gate decides.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

TOLERANCE = 3
TIMEOUT_S = 180
HERE = Path(__file__).resolve().parent


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, color, interlace) != (8, 6, 0):
                raise ValueError(f"{path}: want 8-bit RGBA non-interlaced, got depth {depth} color {color}")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return width, height, b"".join(rows)


def write_png(path, width, height, rgba):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


@dataclass
class Result:
    scene: str
    status: str
    detail: str = ""

    @property
    def passed(self):
        return self.status == "pass"


def compare(out_png, ref_png, diff_png):
    w, h, got = read_png(out_png)
    rw, rh, want = read_png(ref_png)
    if (w, h) != (rw, rh):
        return Result("", "mismatch", f"size {w}x{h}, reference {rw}x{rh}")
    bad, worst, mask = 0, 0, bytearray()
    for i in range(0, len(got), 4):
        d = max(abs(got[i + k] - want[i + k]) for k in range(4))
        worst = max(worst, d)
        if d > TOLERANCE:
            bad += 1
            mask += b"\xff\x00\x00\xff"
        else:
            g = (got[i] + got[i + 1] + got[i + 2]) // 12
            mask += bytes((g, g, g, 255))
    if bad:
        write_png(diff_png, w, h, bytes(mask))
        return Result("", "mismatch", f"{bad} px over tolerance {TOLERANCE}, max diff {worst}")
    return Result("", "pass", f"max diff {worst}")


def classify_exit(code, stdout, stderr):
    lines = [l for l in stderr.splitlines() if l.strip()] or [l for l in stdout.splitlines() if l.strip()]
    first = next((l for l in lines if l.startswith(("CRASH", "ABORT"))), lines[-1] if lines else "")
    if code == 4:
        return "probe", first
    if code in (1, 2, 3):
        return "error", f"exit {code}: {first}"
    if code == 134:
        return "abort", first
    return "crash", f"exit 0x{code & 0xffffffff:08X}: {first}"


def run_scene(exe, scene, out, ref_dir, renderer, env):
    log = out / f"{scene}.log"
    try:
        proc = subprocess.run([str(exe), "--scene", scene, "--out", str(out)], capture_output=True, text=True,
                              errors="replace", timeout=TIMEOUT_S,
                              env={**env, "GALLIUM_LOG_FILE": str(out / f"{scene}.mesa.log")})
    except subprocess.TimeoutExpired as e:
        log.write_text(f"timeout after {TIMEOUT_S}s\n{e.stdout or ''}{e.stderr or ''}")
        return Result(scene, "timeout", f"no exit after {TIMEOUT_S}s")
    log.write_text(f"exit {proc.returncode}\n--- stdout\n{proc.stdout}--- stderr\n{proc.stderr}")
    found = next((l[len("renderer="):] for l in proc.stdout.splitlines() if l.startswith("renderer=")), None)
    if (found is None and proc.returncode == 0) or (found is not None and renderer not in found):
        return Result(scene, "wrong-renderer", found or "glprobe printed no renderer= line")
    if proc.returncode != 0:
        status, detail = classify_exit(proc.returncode, proc.stdout, proc.stderr)
        return Result(scene, status, detail)
    ref = ref_dir / f"{scene}.png"
    if not ref.exists():
        return Result(scene, "no-ref", f"{ref} missing")
    r = compare(out / f"{scene}.png", ref, out / "diff" / f"{scene}.png")
    return Result(scene, r.status, r.detail)


def read_list(path):
    return [l.split("#")[0].strip() for l in path.read_text().splitlines() if l.split("#")[0].strip()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--ref", type=Path, default=HERE / "ref")
    ap.add_argument("--renderer", default="D3D12", help="substring GL_RENDERER must contain")
    ap.add_argument("--title", default="glprobe")
    gate = ap.add_mutually_exclusive_group(required=True)
    gate.add_argument("--expect-all", action="store_true", help="fail unless every scene passes")
    gate.add_argument("--expect", type=Path, help="fail unless every scene listed in this file passes")
    ap.add_argument("--write-ref", type=Path, help="copy every image glprobe wrote into this directory")
    ap.add_argument("--scene", action="append", help="run only these scenes")
    args = ap.parse_args()
    args.exe = args.exe.resolve()

    listing = subprocess.run([str(args.exe), "--list"], capture_output=True, text=True, check=True).stdout
    scenes = {}
    for line in listing.splitlines():
        if line.strip():
            name, _, feature = line.partition("\t")
            scenes[name] = feature
    expected = set(scenes) if args.expect_all else set(read_list(args.expect))
    unknown = sorted(expected - set(scenes))
    if unknown:
        sys.exit(f"{args.expect}: no such scenes: {', '.join(unknown)}")

    (args.out / "diff").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    results = []
    for scene in (args.scene or scenes):
        r = run_scene(args.exe, scene, args.out, args.ref, args.renderer, env)
        results.append(r)
        print(f"{scene:28} {r.status:14} {r.detail}", flush=True)
        if args.write_ref and (args.out / f"{scene}.png").exists() and r.status in ("pass", "mismatch", "no-ref"):
            args.write_ref.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.out / f"{scene}.png", args.write_ref / f"{scene}.png")

    failing = [r for r in results if r.scene in expected and not r.passed]
    rows = [f"## {args.title}", "", f"{sum(r.passed for r in results)}/{len(results)} pass, "
            f"{len(failing)} required scene(s) failing", "", "| scene | gate | status | detail |", "|---|---|---|---|"]
    for r in results:
        gate_col = ("REQUIRED" if r.scene in expected else "")
        rows.append(f"| {r.scene} | {gate_col} | {r.status} | {r.detail.replace('|', '/')} |")
    table = "\n".join(rows) + "\n"
    print("\n" + table)
    (args.out / "results.md").write_text(table)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(table + "\n")
    if failing:
        print("required scenes failing: " + ", ".join(r.scene for r in failing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
