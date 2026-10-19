#!/usr/bin/env python3
"""Turns the GLSL Mesa captured (MESA_SHADER_CAPTURE_PATH, piglit .shader_test files) into SM 5.1 HLSL
through glslang and SPIRV-Cross, the oracle route the DXBC backend is compared with.

  oracle_convert.py --capture DIR --out DIR

Writes <out>/<scene>-<program>-<stage>.hlsl for every stage that converts, <out>/convert.md with one row
per stage (ok, or which tool refused it), and appends the table to GITHUB_STEP_SUMMARY.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

STAGES = {"vertex": "vert", "fragment": "frag", "geometry": "geom", "tessellation control": "tesc",
          "tessellation evaluation": "tese", "compute": "comp"}


def split(text):
    parts = {}
    current = None
    for line in text.splitlines():
        m = re.match(r"\[(.+) shader\]$", line)
        if m:
            current = m.group(1)
            parts[current] = []
        elif line.startswith("["):
            current = None
        elif current is not None:
            parts[current].append(line)
    return {k: "\n".join(v).strip() + "\n" for k, v in parts.items() if "".join(v).strip()}


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    return p.returncode == 0, (p.stdout + p.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    for test in sorted(args.capture.rglob("*.shader_test")):
        scene = test.parent.name
        program = test.stem
        for stage, ext in STAGES.items():
            parts = split(test.read_text(errors="replace"))
            if stage not in parts:
                continue
            base = args.out / f"{scene}-{program}-{ext}"
            glsl = base.with_suffix(f".{ext}")
            glsl.write_text(parts[stage])
            spv = base.with_suffix(".spv")
            ok, log = run(["glslangValidator", "-G", "--auto-map-locations", "--auto-map-bindings", "-o", str(spv), str(glsl)])
            if not ok:
                rows.append((scene, program, ext, "glslang refused", log.splitlines()[-1][:160] if log else ""))
                continue
            hlsl = base.with_suffix(".hlsl")
            ok, log = run(["spirv-cross", "--hlsl", "--shader-model", "51", "--output", str(hlsl), str(spv)])
            if not ok:
                rows.append((scene, program, ext, "spirv-cross refused", log.splitlines()[-1][:160] if log else ""))
                continue
            rows.append((scene, program, ext, "ok", f"{hlsl.stat().st_size} bytes"))
    lines = ["## Oracle route: GLSL to HLSL (glslang + SPIRV-Cross)", "", "| scene | program | stage | result | detail |",
             "|---|---|---|---|---|"]
    lines += [f"| {s} | {p} | {e} | {r} | {d.replace('|', '/')} |" for s, p, e, r, d in rows]
    n_ok = sum(1 for r in rows if r[3] == "ok")
    lines += ["", f"{n_ok}/{len(rows)} stages converted to SM 5.1 HLSL"]
    text = "\n".join(lines) + "\n"
    (args.out / "convert.md").write_text(text)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
