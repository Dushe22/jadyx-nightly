# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only inventory of exported Jadyx caches: the DXIL disk cache (d3d12_dxil.bin) and
JIT prewarm profiles (core-N.bin, a32-core-N.bin). Files are opened "rb" only and never
repaired, truncated or rewritten; payloads are streamed in bounded chunks.

Formats mirror src/video_core/renderer_d3d12/d3d12_dxil_disk_cache.cpp,
d3d12_shader_compiler.cpp (EncodeStages) and src/core/arm/dynarmic/jit_prewarm_profile.h.
"""

import argparse
import json
from pathlib import Path
import struct
import sys

CHUNK = 1 << 20
M64 = (1 << 64) - 1

# ---- DXIL disk cache ------------------------------------------------------------------
DXIL_FILE_MAGIC = 0x4358444A  # "JDXC"
DXIL_FILE_VERSION = 2
DXIL_RECORD_MAGIC = 0x5258444A  # "JDXR"
DXIL_MAX_RECORD = 64 << 20
DXIL_STAGE_LIMIT = 8  # shortcut: sanity bound, EDEN_SPIRV_TO_DXIL_MAX_STAGES is not exported here

# ---- JIT prewarm profiles -------------------------------------------------------------
JIT_HEADER = 8 + 8 + 32 + 4 + 4 + 8
JIT_MAX_RECORDS = 524288
JIT_MAX_CODE_BYTES = 64 * 1024
FNV_SEED = 14695981039346656037
FNV_PRIME = 1099511628211
LAYOUTS = {
    b"EDJITP02": ("a64", False, (1 << 56) - 1, 0x07c80000 << 37, 0),
    b"EDJITP01": ("a64", True, (1 << 56) - 1, 0x07c80000 << 37, 0),
    b"EDJA3201": ("a32", False, 0xFFFFFFFF, 0x07F7FF03 << 32, 1 << 32),
}

# ---- CityHash64 (src/common/cityhash.cpp), streamed --------------------------------------
K0, K1, K2 = 0xC3A5C85C97CB3127, 0xB492B66FBE98F273, 0x9AE16A3B2F90404F


def _rot(v, s):
    return v if s == 0 else ((v >> s) | (v << (64 - s))) & M64


def _smix(v):
    return v ^ (v >> 47)


def _f64(b, o):
    return int.from_bytes(b[o:o + 8], "little")


def _swap(v):
    return int.from_bytes(v.to_bytes(8, "little"), "big")


def _h16(u, v, mul=0x9DDFEA08EB382D69):
    a = ((u ^ v) * mul) & M64
    a ^= a >> 47
    b = ((v ^ a) * mul) & M64
    b ^= b >> 47
    return (b * mul) & M64


def _weak(s, o, a, b):
    w, x, y, z = _f64(s, o), _f64(s, o + 8), _f64(s, o + 16), _f64(s, o + 24)
    a = (a + w) & M64
    b = _rot((b + a + z) & M64, 21)
    c = a
    a = (a + x + y) & M64
    b = (b + _rot(a, 44)) & M64
    return (a + z) & M64, (b + c) & M64


def _city_small(s):
    n = len(s)
    if n <= 16:
        if n >= 8:
            mul = (K2 + n * 2) & M64
            a = (_f64(s, 0) + K2) & M64
            b = _f64(s, n - 8)
            c = (_rot(b, 37) * mul + a) & M64
            d = ((_rot(a, 25) + b) * mul) & M64
            return _h16(c, d, mul)
        if n >= 4:
            mul = (K2 + n * 2) & M64
            a = int.from_bytes(s[0:4], "little")
            return _h16((n + (a << 3)) & M64, int.from_bytes(s[n - 4:n], "little"), mul)
        if n > 0:
            y = s[0] + (s[n >> 1] << 8)
            z = n + (s[n - 1] << 2)
            return (_smix(((y * K2) ^ (z * K0)) & M64) * K2) & M64
        return K2
    if n <= 32:
        mul = (K2 + n * 2) & M64
        a = (_f64(s, 0) * K1) & M64
        b = _f64(s, 8)
        c = (_f64(s, n - 8) * mul) & M64
        d = (_f64(s, n - 16) * K2) & M64
        return _h16((_rot((a + b) & M64, 43) + _rot(c, 30) + d) & M64,
                    (a + _rot((b + K2) & M64, 18) + c) & M64, mul)
    mul = (K2 + n * 2) & M64
    a = (_f64(s, 0) * K2) & M64
    b = _f64(s, 8)
    c = _f64(s, n - 24)
    d = _f64(s, n - 32)
    e = (_f64(s, 16) * K2) & M64
    f = (_f64(s, 24) * 9) & M64
    g = _f64(s, n - 8)
    h = (_f64(s, n - 16) * mul) & M64
    u = (_rot((a + g) & M64, 43) + (_rot(b, 30) + c) * 9) & M64
    v = (((a + g) ^ d) + f + 1) & M64
    w = (_swap((u + v) * mul & M64) + h) & M64
    x = (_rot((e + f) & M64, 42) + c) & M64
    y = ((_swap((v + w) * mul & M64) + g) * mul) & M64
    z = (e + f + c) & M64
    a = (_swap(((x + z) * mul + y) & M64) + b) & M64
    b = (_smix(((z + a) * mul + d + h) & M64) * mul) & M64
    return (b + x) & M64


def city_hash64_file(f, offset, length):
    """CityHash64 of f[offset:offset+length] holding at most CHUNK + 64 bytes at a time."""
    if length <= 64:
        f.seek(offset)
        return _city_small(f.read(length))
    f.seek(offset + length - 64)
    tail = f.read(64)  # tail[i] == s[length - 64 + i]
    x = _f64(tail, 24)
    y = (_f64(tail, 48) + _f64(tail, 8)) & M64
    z = _h16((_f64(tail, 16) + length) & M64, _f64(tail, 40))
    v = _weak(tail, 0, length & M64, z)
    w = _weak(tail, 32, (y + K1) & M64, x)
    f.seek(offset)
    remaining = (length - 1) & ~63
    first = True
    while remaining:
        block = f.read(min(CHUNK, remaining))
        if first:
            x = (x * K1 + _f64(block, 0)) & M64
            first = False
        for o in range(0, len(block), 64):
            x = (_rot((x + y + v[0] + _f64(block, o + 8)) & M64, 37) * K1) & M64
            y = (_rot((y + v[1] + _f64(block, o + 48)) & M64, 42) * K1) & M64
            x ^= w[1]
            y = (y + v[0] + _f64(block, o + 40)) & M64
            z = (_rot((z + w[0]) & M64, 33) * K1) & M64
            v = _weak(block, o, (v[1] * K1) & M64, (x + w[0]) & M64)
            w = _weak(block, o + 32, (z + w[1]) & M64, (y + _f64(block, o + 16)) & M64)
            z, x = x, z
        remaining -= len(block)
    return _h16((_h16(v[0], w[0]) + _smix(y) * K1 + z) & M64, (_h16(v[1], w[1]) + x) & M64)


# ---- DXIL ------------------------------------------------------------------------------
def _stages(f, offset, size):
    """EncodeStages layout; the stream-output tail has no length, so leftovers stay unknown."""
    f.seek(offset)
    head = f.read(4)
    if len(head) < 4:
        return {"status": "truncated"}
    count = struct.unpack("<I", head)[0]
    if count == 0 or count > DXIL_STAGE_LIMIT:
        return {"status": "invalid_stage_count", "count": count}
    stages, pos = [], 4
    for _ in range(count):
        if pos + 6 > size:
            return {"status": "truncated", "count": count, "stages": stages}
        f.seek(offset + pos)
        dxil_size, runtime_data, sysvals = struct.unpack("<IBB", f.read(6))
        pos += 6
        if dxil_size > size - pos:
            return {"status": "stage_overflow", "count": count, "stages": stages}
        f.seek(offset + pos)
        fourcc = f.read(4)
        stages.append({"dxil_bytes": dxil_size, "container": fourcc.decode("latin-1")
                       if fourcc == b"DXBC" else "unknown",
                       "requires_runtime_data": bool(runtime_data), "needs_draw_sysvals": bool(sysvals)})
        pos += dxil_size
    return {"status": "valid", "count": count, "stages": stages,
            # A stream_output struct, if the pipeline had one; its size is not known offline.
            "trailing_bytes": size - pos}


def inspect_dxil(path, f, file_size, verify):
    report = {"path": str(path), "kind": "dxil_disk_cache", "bytes": file_size,
              "title": None, "build": None, "core": None,
              "title_note": "not stored in the file; the directory name is the title id when exported intact"}
    if file_size < 16:
        report["structure"] = {"status": "truncated_header"}
        return report
    magic, version, generation = struct.unpack("<IIQ", f.read(16))
    report.update(format="JDXC", version=version, generation=f"{generation:016x}")
    if magic != DXIL_FILE_MAGIC:
        report["structure"] = {"status": "bad_magic"}
        return report
    if version != DXIL_FILE_VERSION:
        report["structure"] = {"status": "unsupported_version"}
        return report
    records, offset, stop = [], 16, None
    while offset + 32 <= file_size:
        f.seek(offset)
        rmagic, size, lo, hi, checksum = struct.unpack("<IIQQQ", f.read(32))
        if rmagic != DXIL_RECORD_MAGIC:
            stop = "bad_record_magic"
            break
        if size > DXIL_MAX_RECORD:
            stop = "record_size_over_limit"
            break
        if offset + 32 + size > file_size:
            stop = "record_truncated"
            break
        rec = {"offset": offset, "payload_bytes": size, "key": f"{hi:016x}{lo:016x}",
               "stored_checksum": f"{checksum:016x}", "payload": _stages(f, offset + 32, size)}
        if verify:
            rec["checksum"] = "valid" if city_hash64_file(f, offset + 32, size) == checksum else "mismatch"
        else:
            rec["checksum"] = "not_checked"
        records.append(rec)
        offset += 32 + size
    tail = file_size - offset
    if stop is None and tail:
        stop = "partial_record_header"
    # Open() keeps the last record per key (insert_or_assign); earlier ones are unreachable.
    last = {}
    for i, rec in enumerate(records):
        last[rec["key"]] = i
    for i, rec in enumerate(records):
        rec["superseded"] = last[rec["key"]] != i
    same_checksum = {}
    for rec in records:
        same_checksum.setdefault((rec["stored_checksum"], rec["payload_bytes"]), []).append(rec["key"])
    report["structure"] = {
        "status": "valid" if stop is None else "tail_invalid",
        "stop_reason": stop, "indexed_end": offset, "unindexed_tail_bytes": tail,
        "note": "the runtime Open() would truncate this tail; this tool leaves the file as is"}
    report["records"] = records
    report["summary"] = {
        "records": len(records), "unique_keys": len(last),
        "superseded_records": len(records) - len(last),
        "payload_bytes": sum(r["payload_bytes"] for r in records),
        "checksum_mismatches": sum(r["checksum"] == "mismatch" for r in records) if verify else None,
        "payload_invalid": sum(r["payload"]["status"] != "valid" for r in records),
        # Same stored hash and size suggests, but does not prove, identical bytes.
        "same_checksum_groups": [k for k in same_checksum.values() if len(set(k)) > 1]}
    report["relations"] = {
        "known": ["key = CityHash128 over translator id, options and each stage's SPIR-V",
                  "generation = translator id ^ DXIL_CACHE_FORMAT; a mismatch makes the runtime start over"],
        "unknown": ["which PSO, draw or game area uses a record",
                    "program validation: needs the SPIR-V that produced the key, not available offline"]}
    report["program_validation"] = "not_available"
    return report


# ---- JIT -------------------------------------------------------------------------------
def _fnv(h, value, n):
    for _ in range(n):
        h = ((h ^ (value & 0xFF)) * FNV_PRIME) & M64
        value >>= 8
    return h


def inspect_jit(path, f, file_size, verify):
    report = {"path": str(path), "kind": "jit_prewarm_profile", "bytes": file_size}
    if file_size < JIT_HEADER:
        report["structure"] = {"status": "truncated_header"}
        return report
    head = f.read(JIT_HEADER)
    magic = head[:8]
    title, = struct.unpack_from("<Q", head, 8)
    build = head[16:48]
    core, count, stored_hash = struct.unpack_from("<IIQ", head, 48)
    report.update(format=magic.decode("latin-1"), title=f"{title:016x}", build=build.hex(), core=core)
    if magic not in LAYOUTS:
        report["structure"] = {"status": "bad_magic"}
        return report
    isa, legacy, pc_mask, state_mask, half_bit = LAYOUTS[magic]
    rbytes = 20 if legacy else 24
    report.update(isa=isa, version="legacy" if legacy else "current")
    if count > JIT_MAX_RECORDS:
        report["structure"] = {"status": "count_over_limit", "count": count}
        return report
    expected = JIT_HEADER + count * rbytes
    if file_size != expected:
        report["structure"] = {"status": "truncated" if file_size < expected else "trailing_bytes",
                               "count": count, "expected_bytes": expected}
        return report
    h, prev, bad, unordered, code_bytes, sampled = FNV_SEED, None, None, None, 0, 0
    per = max(1, CHUNK // rbytes)
    for start in range(0, count, per):
        block = f.read(min(per, count - start) * rbytes)
        for i in range(len(block) // rbytes):
            o = i * rbytes
            desc, code_hash, nbytes = struct.unpack_from("<QQI", block, o)
            samples = 0 if legacy else struct.unpack_from("<I", block, o + 20)[0]
            align = 1 if desc & half_bit else 3
            if bad is None and not (0 < nbytes <= JIT_MAX_CODE_BYTES and nbytes & 3 == 0 and
                                    desc & align == 0 and desc & ~(pc_mask | state_mask) & M64 == 0):
                bad = start + i
            if unordered is None and prev is not None and prev >= desc:
                unordered = start + i  # also catches duplicate descriptors
            prev = desc
            code_bytes += nbytes
            sampled += samples != 0
            if verify:
                h = _fnv(_fnv(_fnv(h, desc, 8), code_hash, 8), nbytes, 4)
                if not legacy:
                    h = _fnv(h, samples, 4)
    ok = bad is None and unordered is None
    report["structure"] = {"status": "valid" if ok else "invalid_records", "count": count,
                           "first_invalid_record": bad, "first_unordered_or_duplicate": unordered,
                           "note": "the runtime rejects the whole file on any invalid record"}
    report["checksum"] = ("not_checked" if not verify else
                          "valid" if h == stored_hash else "mismatch")
    report["summary"] = {"records": count, "guest_code_bytes_described": code_bytes,
                         "records_with_gameplay_samples": None if legacy else sampled}
    report["relations"] = {
        "known": ["descriptor PC is relative to the module base; state bits select the translation",
                  "code_hash/code_bytes fingerprint the guest words a block was translated from",
                  "title, build id and core must match the session or the runtime ignores the file"],
        "unknown": ["host code: profiles hold no native code, nothing here is resident",
                    "game area, real priority or full coverage: samples only count observations"]}
    report["program_validation"] = "not_available: needs the guest executable loaded at its base"
    return report


def inspect(path, verify=True):
    path = Path(path)
    with open(path, "rb") as f:
        size = f.seek(0, 2)
        f.seek(0)
        lead = f.read(8)
        f.seek(0)
        if lead[:4] == struct.pack("<I", DXIL_FILE_MAGIC):
            return inspect_dxil(path, f, size, verify)
        if lead in LAYOUTS:
            return inspect_jit(path, f, size, verify)
        return {"path": str(path), "kind": "unknown", "bytes": size,
                "structure": {"status": "unrecognized_magic"}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="+", type=Path, help="cache files or directories")
    parser.add_argument("--output", type=Path, help="JSON file (default: stdout)")
    parser.add_argument("--no-checksum", action="store_true", help="skip payload hashing")
    args = parser.parse_args(argv)
    files = []
    for p in args.paths:
        files += sorted(q for q in p.rglob("*.bin") if q.is_file()) if p.is_dir() else [p]
    reports = [inspect(p, not args.no_checksum) for p in files]
    text = json.dumps({"tool": "inspect-caches", "files": reports}, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    # Other .bin files in a scanned directory (PSO library, hot set) are listed, not failures.
    return 0 if all(r["kind"] == "unknown" or r["structure"]["status"] == "valid" and r.get("checksum", "valid") != "mismatch"
                    and not (r.get("summary") or {}).get("checksum_mismatches") for r in reports) else 1


if __name__ == "__main__":
    sys.exit(main())
