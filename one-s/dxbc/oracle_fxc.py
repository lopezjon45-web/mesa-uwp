#!/usr/bin/env python3
"""Compiles the oracle HLSL (oracle_convert.py output) with fxc.exe for SM 5.1 and tabulates compile time,
acceptance and instruction count, the numbers the DXBC backend's own compile times are compared with.

  oracle_fxc.py --hlsl DIR --out DIR

Each .hlsl is named <scene>-<program>-<ext>.hlsl; ext picks the target profile. SPIRV-Cross names the
entry point "main". fxc is found under the Windows SDK bin directory (x64).
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

PROFILES = {"vert": "vs_5_1", "frag": "ps_5_1", "geom": "gs_5_1", "tesc": "hs_5_1", "tese": "ds_5_1", "comp": "cs_5_1"}


def find_fxc():
    sdk = os.environ.get("WindowsSdkDir") or r"C:\Program Files (x86)\Windows Kits\10"
    found = sorted(Path(sdk, "bin").rglob("x64/fxc.exe"))
    if not found:
        sys.exit("fxc.exe not found under " + sdk)
    return found[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hlsl", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    fxc = find_fxc()
    rows = []
    for hlsl in sorted(args.hlsl.rglob("*.hlsl")):
        ext = hlsl.stem.rsplit("-", 1)[-1]
        profile = PROFILES.get(ext)
        if not profile:
            continue
        blob = args.out / (hlsl.stem + ".dxbc")
        asm = args.out / (hlsl.stem + ".asm")
        t0 = time.perf_counter()
        p = subprocess.run([str(fxc), "/nologo", "/T", profile, "/E", "main", "/Fo", str(blob), "/Fc", str(asm), str(hlsl)],
                           capture_output=True, text=True, errors="replace")
        ms = (time.perf_counter() - t0) * 1000
        if p.returncode != 0:
            err = (p.stderr or p.stdout).strip().splitlines()
            rows.append((hlsl.stem, profile, "refused", ms, "", (err[-1] if err else "")[:160]))
            continue
        text = asm.read_text(errors="replace")
        m = re.search(r"Approximately (\d+) instruction slots used", text)
        count = m.group(1) if m else str(sum(1 for l in text.splitlines() if l and not l.startswith(("//", "dcl_", " ")) and ":" not in l))
        rows.append((hlsl.stem, profile, "ok", ms, count, f"{blob.stat().st_size} bytes"))
    lines = ["## Oracle route: FXC (SM 5.1) on the converted HLSL", "",
             "| shader | profile | result | fxc ms (process) | instructions | detail |", "|---|---|---|---|---|---|"]
    lines += [f"| {n} | {pf} | {r} | {ms:.0f} | {c} | {d.replace('|', '/')} |" for n, pf, r, ms, c, d in rows]
    ok = [r for r in rows if r[2] == "ok"]
    if ok:
        xs = sorted(r[3] for r in ok)
        lines += ["", f"{len(ok)}/{len(rows)} compiled; fxc process time median {xs[len(xs) // 2]:.0f} ms, max {xs[-1]:.0f} ms "
                  "(includes process start, so an upper bound on compile time)"]
    text = "\n".join(lines) + "\n"
    (args.out / "fxc.md").write_text(text)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
