#!/usr/bin/env python3
"""Runs glprobe scenes through one-s/glprobe/run.py three times and collects what the DXBC work needs.

  collect.py --exe glprobe.exe --out DIR --scenes a,b,c

  DIR/dxil/      the DXIL path; its images become the references for the other two passes, and the
                 GLSL of every program lands under DIR/dxil/capture/<scene>/ (MESA_SHADER_CAPTURE_PATH)
  DIR/sm51/      D3D12_MAX_SHADER_MODEL=51 (the One S), compared against DIR/dxil, every blob the
                 driver compiled dumped under DIR/sm51/blobs/<scene>/, compile times per shader
  DIR/sm50/      D3D12_MAX_SHADER_MODEL=50, blobs only (SM 5.0 is what D3D11 FL 11_0 accepts)
  DIR/summary.md compile-time table and pass counts, also appended to GITHUB_STEP_SUMMARY

Each scene runs in its own working directory because the driver names dumps dump00.dxil,
dump01.dxil, ... per process.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUN_PY = HERE.parent / "glprobe" / "run.py"
TIMING = re.compile(r"D3D12: compiled (\w+) (\w+) (\d+) bytes in ([0-9.]+) ms")


def run_pass(exe, out, scenes, ref, env_extra, dump, capture=False):
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    for scene in scenes:
        env = {**os.environ, **env_extra}
        cwd = out / "blobs" / scene if dump else out
        cwd.mkdir(parents=True, exist_ok=True)
        if capture:
            (out / "capture" / scene).mkdir(parents=True, exist_ok=True)
            env["MESA_SHADER_CAPTURE_PATH"] = str((out / "capture" / scene).resolve())
        cmd = [sys.executable, str(RUN_PY), "--exe", str(exe.resolve()), "--out", str(out.resolve()),
               "--scene", scene, "--title", scene]
        if ref:
            cmd += ["--ref", str(ref.resolve())]
        else:
            cmd += ["--write-ref", str((out / "ref").resolve())]
        proc = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
        line = next((l for l in proc.stdout.splitlines() if l.startswith(scene)), "")
        results[scene] = line.split()[1] if len(line.split()) > 1 else "no-output"
        print(f"[{out.name}] {line or proc.stdout.strip()[-200:]}", flush=True)
    return results


def timings(out, scenes):
    rows = []
    for scene in scenes:
        log = out / f"{scene}.mesa.log"
        if not log.exists():
            continue
        for m in TIMING.finditer(log.read_text(errors="replace")):
            rows.append((scene, m.group(1), m.group(2), int(m.group(3)), float(m.group(4))))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--scenes", required=True, help="comma-separated scene names")
    args = ap.parse_args()
    scenes = [s for s in args.scenes.split(",") if s]

    dxil = run_pass(args.exe, args.out / "dxil", scenes, None, {"D3D12_DEBUG": "verbose"}, dump=False, capture=True)
    sm51 = run_pass(args.exe, args.out / "sm51", scenes, args.out / "dxil" / "ref",
                    {"D3D12_MAX_SHADER_MODEL": "51", "D3D12_DEBUG": "dxil,verbose"}, dump=True)
    sm50 = run_pass(args.exe, args.out / "sm50", scenes, args.out / "dxil" / "ref",
                    {"D3D12_MAX_SHADER_MODEL": "50", "D3D12_DEBUG": "dxil"}, dump=True)

    lines = ["## DXBC port: scene results", "", "| scene | DXIL | SM 5.1 vs DXIL | SM 5.0 vs DXIL |", "|---|---|---|---|"]
    for s in scenes:
        lines.append(f"| {s} | {dxil.get(s)} | {sm51.get(s)} | {sm50.get(s)} |")
    passing = [s for s in scenes if sm51.get(s) == "pass"]
    lines += ["", f"SM 5.1 scenes matching the DXIL image: {len(passing)}/{len(scenes)}"
              + (" (" + ", ".join(passing) + ")" if passing else ""), ""]

    lines += ["## Compile time per shader (ms, one process per scene, debug build, WARP host)", "",
              "| scene | stage | backend | bytes | ms |", "|---|---|---|---|---|"]
    rows = timings(args.out / "dxil", scenes) + timings(args.out / "sm51", scenes)
    for scene, stage, backend, nbytes, ms in rows:
        lines.append(f"| {scene} | {stage} | {backend} | {nbytes} | {ms:.2f} |")
    by_backend = {}
    for _, _, backend, _, ms in rows:
        by_backend.setdefault(backend, []).append(ms)
    for backend, xs in sorted(by_backend.items()):
        xs.sort()
        lines.append(f"\n{backend}: n={len(xs)} median {xs[len(xs) // 2]:.2f} ms, max {xs[-1]:.2f} ms")
    text = "\n".join(lines) + "\n"
    (args.out / "summary.md").write_text(text)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
