# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "inspect_caches", Path(__file__).resolve().parents[1] / "inspect-caches.py")
ic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ic)

# Produced by Common::CityHash64 (src/common/cityhash.cpp) over the LCG bytes below.
CITY_VECTORS = {0: 0x9ae16a3b2f90404f, 7: 0xcd6452948d74f843, 17: 0xf37241559f59a08c,
                33: 0x2a6472b9e35f616d, 64: 0xb2327bdeb8eb139a, 65: 0xba98866f4b4d5844,
                129: 0x8b7c563c85d3d94c, 1000: 0xf92068667fa63feb,
                (1 << 20) + 3: 0x8809bf79c89556d8, (1 << 21) + 64: 0xaec42e4caa4ca2dc}


def lcg(n):
    x, out = 12345, bytearray(n)
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def city(data):
    return ic.city_hash64_file(io.BytesIO(data), 0, len(data))


def dxil_payload(stages=(b"DXBC" + bytes(28),), tail=b""):
    out = struct.pack("<I", len(stages))
    for s in stages:
        out += struct.pack("<IBB", len(s), 1, 0) + s
    return out + tail


def dxil_record(key, payload, checksum=None):
    return struct.pack("<IIQQQ", ic.DXIL_RECORD_MAGIC, len(payload), key, 0,
                       city(payload) if checksum is None else checksum) + payload


def dxil_file(*records, generation=0x1234, version=2):
    return struct.pack("<IIQ", ic.DXIL_FILE_MAGIC, version, generation) + b"".join(records)


def jit_file(records, magic=b"EDJITP02", core=1, title=0x0100000000010000, count=None, hash_=None):
    legacy = magic == b"EDJITP01"
    h, body = ic.FNV_SEED, b""
    for desc, code_hash, nbytes, samples in records:
        h = ic._fnv(ic._fnv(ic._fnv(h, desc, 8), code_hash, 8), nbytes, 4)
        body += struct.pack("<QQI", desc, code_hash, nbytes)
        if not legacy:
            h = ic._fnv(h, samples, 4)
            body += struct.pack("<I", samples)
    return (magic + struct.pack("<Q", title) + bytes(range(32)) +
            struct.pack("<IIQ", core, len(records) if count is None else count,
                        h if hash_ is None else hash_) + body)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def check(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        before = hashlib.sha256(data).hexdigest()
        report = ic.inspect(path)
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), before, "input changed")
        return report


class CityHashTests(unittest.TestCase):
    def test_matches_native_vectors_across_chunk_boundaries(self):
        for n, expected in CITY_VECTORS.items():
            self.assertEqual(city(lcg(n)), expected, n)


class DxilTests(Fixture):
    def test_valid_file_with_duplicates_and_superseded_records(self):
        p = dxil_payload()
        data = dxil_file(dxil_record(1, p), dxil_record(2, p), dxil_record(1, dxil_payload(tail=b"so")))
        r = self.check("d3d12_dxil.bin", data)
        self.assertEqual(r["structure"]["status"], "valid")
        self.assertEqual(r["generation"], "0000000000001234")
        self.assertIsNone(r["title"])
        self.assertEqual([x["superseded"] for x in r["records"]], [True, False, False])
        self.assertEqual(r["summary"]["unique_keys"], 2)
        self.assertEqual(r["summary"]["checksum_mismatches"], 0)
        self.assertEqual(r["records"][2]["payload"]["trailing_bytes"], 2)
        self.assertEqual(r["records"][0]["payload"]["stages"][0]["container"], "DXBC")
        self.assertEqual(len(r["summary"]["same_checksum_groups"]), 1)
        self.assertEqual(r["program_validation"], "not_available")

    def test_checksum_mismatch_is_separate_from_structure(self):
        r = self.check("c.bin", dxil_file(dxil_record(1, dxil_payload(), checksum=7)))
        self.assertEqual(r["structure"]["status"], "valid")
        self.assertEqual(r["records"][0]["checksum"], "mismatch")

    def test_truncated_tail_is_reported_not_repaired(self):
        data = dxil_file(dxil_record(1, dxil_payload()), dxil_record(2, dxil_payload()))[:-5]
        r = self.check("t.bin", data)
        self.assertEqual(r["structure"]["stop_reason"], "record_truncated")
        self.assertEqual(r["summary"]["records"], 1)
        self.assertGreater(r["structure"]["unindexed_tail_bytes"], 0)

    def test_limits_and_bad_headers(self):
        huge = struct.pack("<IIQQQ", ic.DXIL_RECORD_MAGIC, ic.DXIL_MAX_RECORD + 1, 1, 0, 0)
        self.assertEqual(self.check("h.bin", dxil_file(huge))["structure"]["stop_reason"],
                         "record_size_over_limit")
        self.assertEqual(self.check("v.bin", dxil_file(version=3))["structure"]["status"],
                         "unsupported_version")
        self.assertEqual(self.check("s.bin", b"JDXC")["kind"], "dxil_disk_cache")
        self.assertEqual(self.check("s.bin", b"JDXC")["structure"]["status"], "truncated_header")
        overflow = struct.pack("<I", 1) + struct.pack("<IBB", 0xFFFFFFFF, 0, 0)
        r = self.check("o.bin", dxil_file(dxil_record(1, overflow)))
        self.assertEqual(r["records"][0]["payload"]["status"], "stage_overflow")
        self.assertEqual(r["summary"]["payload_invalid"], 1)

    def test_large_payload_hashes_in_chunks(self):
        r = self.check("l.bin", dxil_file(dxil_record(9, dxil_payload((b"DXBC" + lcg(3 << 20),)))))
        self.assertEqual(r["records"][0]["checksum"], "valid")


class JitTests(Fixture):
    A64 = [(0x1000, 0xAA, 16, 3), (0x2000, 0xBB, 8, 0)]

    def test_valid_current_and_legacy(self):
        r = self.check("core-1.bin", jit_file(self.A64))
        self.assertEqual((r["structure"]["status"], r["checksum"], r["isa"]), ("valid", "valid", "a64"))
        self.assertEqual((r["title"], r["core"]), ("0100000000010000", 1))
        self.assertEqual(r["summary"]["records_with_gameplay_samples"], 1)
        legacy = self.check("core-0.bin", jit_file(self.A64, magic=b"EDJITP01"))
        self.assertEqual((legacy["version"], legacy["checksum"]), ("legacy", "valid"))
        a32 = self.check("a32-core-0.bin", jit_file([(0x1002 | 1 << 32, 1, 4, 0)], magic=b"EDJA3201"))
        self.assertEqual(a32["structure"]["status"], "valid")

    def test_corrupt_records_and_checksum(self):
        self.assertEqual(self.check("x.bin", jit_file(self.A64, hash_=1))["checksum"], "mismatch")
        dup = self.check("d.bin", jit_file([self.A64[0], self.A64[0]]))
        self.assertEqual(dup["structure"]["first_unordered_or_duplicate"], 1)
        bad = self.check("b.bin", jit_file([(0x1001, 1, 4, 0)]))  # misaligned A64 PC
        self.assertEqual(bad["structure"]["first_invalid_record"], 0)
        big = self.check("g.bin", jit_file([(0x1000, 1, ic.JIT_MAX_CODE_BYTES + 4, 0)]))
        self.assertEqual(big["structure"]["status"], "invalid_records")

    def test_truncation_and_count_limits(self):
        self.assertEqual(self.check("t.bin", jit_file(self.A64)[:-1])["structure"]["status"], "truncated")
        self.assertEqual(self.check("e.bin", jit_file(self.A64) + b"\0")["structure"]["status"],
                         "trailing_bytes")
        r = self.check("c.bin", jit_file([], count=ic.JIT_MAX_RECORDS + 1))
        self.assertEqual(r["structure"]["status"], "count_over_limit")
        self.assertEqual(self.check("h.bin", b"EDJITP02")["structure"]["status"], "truncated_header")


class CliTests(Fixture):
    def test_directory_scan_writes_json_and_fails_on_damage(self):
        (self.root / "t").mkdir()
        (self.root / "t" / "d3d12_dxil.bin").write_bytes(dxil_file(dxil_record(1, dxil_payload())))
        (self.root / "t" / "core-0.bin").write_bytes(jit_file(JitTests.A64))
        out = self.root / "report.json"
        script = str(Path(ic.__file__))
        ok = subprocess.run([sys.executable, script, str(self.root / "t"), "--output", str(out)],
                            capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertEqual(len(json.loads(out.read_text(encoding="utf-8"))["files"]), 2)
        (self.root / "t" / "core-0.bin").write_bytes(jit_file(JitTests.A64, hash_=1))
        bad = subprocess.run([sys.executable, script, str(self.root / "t")], capture_output=True)
        self.assertEqual(bad.returncode, 1)


if __name__ == "__main__":
    unittest.main()
