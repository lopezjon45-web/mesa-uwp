# One S harness

The `one-s` workflow (`.github/workflows/one-s.yml`) runs on every push to a branch that carries it, so `one-s-dxbc` and any branch that merges it (such as `dxbc-port`) get the same gates. It also runs by hand: `gh workflow run one-s -R lopezjon45-web/mesa-uwp --ref <branch>`.

The `uwp` job builds `libgallium_wgl.dll` and `opengl32.dll` with their PDBs the way the UWP release is built, and uploads them as the `mesa-uwp-one-s` artifact. It fails if either DLL has an instruction the One S CPU (AMD Jaguar) cannot run, unless the function holding it is in the workflow's allow list. Each allowed function has a comment that says which CPU-feature check guards it. The `uwp-gate-report` artifact holds the disassembly of every offending function.

The `desktop` job builds the same tree for desktop Windows (artifact `mesa-desktop-debug`) and runs every glprobe scene (`one-s/glprobe/`) on the runner's WARP adapter, each scene in its own process:

1. On the normal DXIL path, every scene must match its reference in `one-s/glprobe/ref/` within 3 per channel and pass its CPU-computed probe pixels. The exceptions are the scenes in `one-s/glprobe/dxil-known-failures.txt`, which the DXIL path draws wrong. They have no reference, and this step fails if one of them starts passing, so the list cannot go stale.
2. With `D3D12_MAX_SHADER_MODEL=51`, which caps the d3d12 driver at shader model 5.1 as the One S does, only the scenes listed in `one-s/glprobe/expected-dxbc-pass.txt` must pass. This is the ratchet.

Every step prints a per-scene table to the job summary. The `glprobe-results` artifact holds the images, diffs, glprobe logs and Mesa logs.

## Ratchet

Once the capped run shows a scene passing, add its name on its own line in `expected-dxbc-pass.txt`. From then on any change that breaks the scene fails CI. A scene in `dxil-known-failures.txt` passes the capped run on its probes alone.

## References

The DXIL step copies every image whose scene passed its probes to `results/new-ref` in the `glprobe-results` artifact. For a new or changed scene, check the image by eye and commit it under `one-s/glprobe/ref/`.

## Running glprobe locally

On Windows, from a Visual Studio x64 developer prompt, run `one-s\glprobe\build.cmd <mesa>\include`. Put Mesa's `opengl32.dll`, `libgallium_wgl.dll` and the SDK's `dxil.dll` beside `glprobe.exe`. Then run `python one-s/glprobe/run.py --exe glprobe.exe --out results --expect one-s/glprobe/expected-dxbc-pass.txt` with `GALLIUM_DRIVER=d3d12`.

On macOS, `one-s/glprobe/build-macos.sh <mesa>/include` builds glprobe against the system GL 4.1 core profile. Use `--renderer ""` with run.py. This checks a scene's probes on an independent driver.

## Pipeline descriptor dump

With `MESA_D3D12_PSO_DUMP_DIR` set, the d3d12 driver (`src/gallium/drivers/d3d12/d3d12_pso_dump.cpp`) writes every graphics pipeline it creates into that directory before the create call: `pso-NNNN.txt` (`format 1`, then the V0 descriptor, one `key value` line per field, array elements positional as the `#` lines say), `pso-NNNN-rs.bin` (the serialized root signature) and `pso-NNNN-<stage>.dxbc`, and appends the HRESULT after the call; a record without an `hr` line is a create that never returned. A UWP app has no environment of its own, so a file named `pso-dump` inside `MESA_SHADER_CACHE_DIR` selects that directory instead; xemu points the variable into its LocalState, and the console lever deletes `pso-*` there before each run because numbering restarts with every process. The desktop job dumps `xemu_gpuprops` and `transform_feedback` under the cap into `results/pso-dump/` of the `glprobe-results` artifact and runs `one-s/glprobe/check_pso_dump.py` over them: xemu's first draw must carry no stream-output declaration and no input-layout pointer, the capture scene must carry entries. The One S D3D12 probe replays these files on the console.
