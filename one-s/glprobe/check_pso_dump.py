#!/usr/bin/env python3
"""Checks the stream-output and input-layout fields of the pipeline records Mesa dumped (MESA_D3D12_PSO_DUMP_DIR).

    check_pso_dump.py <dump dir of xemu_gpuprops> <dump dir of transform_feedback>

xemu_gpuprops is xemu's first draw and has no transform feedback, so every record of it must carry no
stream-output declaration (NumEntries 0, NumStrides 0, both pointers NULL, no ALLOW_STREAM_OUTPUT on the
root signature) and, having no vertex attributes, a NULL input-layout pointer. transform_feedback captures
varyings, so at least one of its records must carry entries and the flag; that keeps the check from passing
on a driver that dropped stream output altogether. RasterizedStream is printed but not judged: rasterizer
discard sets it without transform feedback. Every record must be "format 1", carry an hr line (a create
that threw leaves none) and hr 0. Every stage blob beside a record must carry signature read/write bytes
as fxc writes them (an input reads a non-empty subset of its mask, an output's never-written byte is four
bits outside its mask): the One S driver refused Mesa's pixel shader whose input byte read 0 (probe 1.3.3.0,
2026-10-25). The stream-output, input-layout and root-signature lines of each record are printed: the
evidence for hypothesis (a) in xemu-dxbc/RESUME.md (2026-10-20). Exit 1 on a violation.
"""
import re
import struct
import sys
from pathlib import Path

ALLOW_STREAM_OUTPUT = 0x40
RECORD = re.compile(r"^pso-(\d+)\.txt$")


def read_record(path):
    rec = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith("#"):
            key, _, value = line.partition(" ")
            rec[key] = value
    return rec


def root_signature_flags(blob):
    """The RTS0 chunk of a serialized root signature: version, parameter count and offset, static sampler
    count and offset, then the flags."""
    if blob[:4] != b"DXBC":
        raise ValueError("not a DXBC container")
    count = struct.unpack_from("<I", blob, 28)[0]
    for off in struct.unpack_from(f"<{count}I", blob, 32):
        if blob[off:off + 4] == b"RTS0":
            return struct.unpack_from("<I", blob, off + 8 + 20)[0]
    raise ValueError("no RTS0 chunk")


SIGNATURE_ELEMENT_SIZE = {b"ISG1": 32, b"OSG1": 32, b"ISGN": 24, b"OSGN": 24, b"PCSG": 24, b"OSG5": 28}
INPUT_TAGS = (b"ISG1", b"ISGN")


def signature_elements(payload, size):
    """(name, index, mask, rw) per element; the 32- and 28-byte layouts put the stream before the 24-byte core."""
    count, off = struct.unpack_from("<II", payload, 0)
    for i in range(count):
        base = off + i * size + (4 if size != 24 else 0)
        name_off, index, _sysval, _ctype, _reg, mask, rw, _ = struct.unpack_from("<IIIIIBBH", payload, base)
        yield payload[name_off:payload.index(b"\0", name_off)].decode(), index, mask, rw


def signature_problems(blob):
    """The read/write byte as every DXBC reader takes it: an input reads a non-empty subset of its mask, an
    output never writes only components outside its mask, four bits. Yields one line per violation."""
    count = struct.unpack_from("<I", blob, 28)[0]
    for off in struct.unpack_from(f"<{count}I", blob, 32):
        tag = blob[off:off + 4]
        if tag not in SIGNATURE_ELEMENT_SIZE:
            continue
        payload = blob[off + 8:off + 8 + struct.unpack_from("<I", blob, off + 4)[0]]
        for name, index, mask, rw in signature_elements(payload, SIGNATURE_ELEMENT_SIZE[tag]):
            if tag in INPUT_TAGS and (rw & ~mask or (mask and not rw)):
                yield f"{tag.decode()} {name}{index}: mask {mask:#x} read byte {rw:#x} (must be a non-empty subset of the mask)"
            if tag not in INPUT_TAGS and (rw & 0xf0 or rw & mask):
                yield f"{tag.decode()} {name}{index}: mask {mask:#x} never-written byte {rw:#x} (must be four bits outside the mask)"


def records(directory):
    found = []
    for txt in Path(directory).iterdir():
        m = RECORD.match(txt.name)
        if m:
            found.append((int(m.group(1)), txt))
    out = []
    for _, txt in sorted(found):
        rec = read_record(txt)
        blob = txt.with_name(txt.stem + "-rs.bin")
        rec["rs.flags"] = f"0x{root_signature_flags(blob.read_bytes()):x}" if blob.exists() else "missing"
        out.append((txt, rec))
    return out


def show(txt, rec):
    print(f"{txt}: format {rec.get('format')} form {rec.get('form')} hr {rec.get('hr', 'none')}")
    for key in sorted(rec):
        if key.startswith(("so.", "ia.", "rs.")):
            print(f"  {key} {rec[key]}")


def record_problems(txt, rec):
    if rec.get("format") != "1":
        yield f"{txt}: format {rec.get('format')}, this check reads format 1"
    if rec.get("hr") != "0x00000000":
        yield f"{txt}: hr {rec.get('hr', 'none')} (none means the create call never returned)"
    if rec["rs.flags"] == "missing":
        yield f"{txt}: no root signature blob beside the record"


def has_stream_output(rec):
    return (int(rec["so.NumEntries"]) > 0 or int(rec["so.NumStrides"]) > 0
            or rec["so.pSODeclaration"] != "null" or rec["so.pBufferStrides"] != "null")


def main(no_xfb_dir, xfb_dir):
    failures = []
    plain = records(no_xfb_dir)
    captures = records(xfb_dir)
    for name, recs in (("xemu_gpuprops", plain), ("transform_feedback", captures)):
        if not recs:
            failures.append(f"{name}: no pso-N.txt under its dump directory")
    for txt, rec in plain + captures:
        show(txt, rec)
        failures += record_problems(txt, rec)
        for stage in ("vs", "hs", "ds", "gs", "ps"):
            blob = txt.with_name(f"{txt.stem}-{stage}.dxbc")
            if blob.exists():
                failures += (f"{blob.name}: {p}" for p in signature_problems(blob.read_bytes()))
    for txt, rec in plain:
        if has_stream_output(rec):
            failures.append(f"{txt}: a stream-output declaration on xemu's first draw, which has no transform feedback")
        if rec["rs.flags"] != "missing" and int(rec["rs.flags"], 16) & ALLOW_STREAM_OUTPUT:
            failures.append(f"{txt}: root signature flags {rec['rs.flags']} carry ALLOW_STREAM_OUTPUT (0x40)")
        if int(rec["ia.NumElements"]) == 0 and rec["ia.pInputElementDescs"] != "null":
            failures.append(f"{txt}: a non-NULL input-layout pointer with no elements")
    if captures and not any(int(rec["so.NumEntries"]) > 0 and rec["rs.flags"] != "missing" and
                            int(rec["rs.flags"], 16) & ALLOW_STREAM_OUTPUT for _, rec in captures):
        failures.append("transform_feedback: no record carries stream-output entries with ALLOW_STREAM_OUTPUT")
    for f in failures:
        print(f"FAIL {f}")
    print(f"check_pso_dump: {len(failures)} failure(s), {len(plain)} + {len(captures)} records")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
