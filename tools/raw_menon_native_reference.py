"""Freeze synthetic uint16/native Menon base fixtures from pinned BSD upstream.

Optional scientific runtime is used only to generate reference data. Engine
tests read the little-endian binary without Python or external dependencies.
"""
import argparse
import itertools
import json
from pathlib import Path
import struct
import sys

import raw_colour_code_review as upstream
import raw_menon_reference as scalar
import raw_photo_remosaic as photo
import raw_quality_harness as h


def generate(source, runtime):
    np, modules, helper = upstream.load_upstream(source, runtime)
    oracle = modules["menon"].demosaicing_CFA_Bayer_Menon2007
    fixtures = []
    for pattern, phase, origin in itertools.product(range(4), ((0, 0), (1, 0), (0, 1), (1, 1)), ((0, 0), (3, 2))):
        fixtures.append((f"signed/{pattern}/{phase}/{origin}", upstream.crosscheck.fixture(pattern, phase, origin)))
    for pattern in range(4):
        fixture = h.generate(next(p for p in h.ALL_PROBES if p.name == "chromatic_affine"),
                             layout="shifted", pattern=pattern, phase=(1, 0), levels="per-site")
        fixtures.append((f"nondyadic/{pattern}", (fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)))
    for shape, pattern, constant in itertools.product(((1, 1), (1, 7), (7, 1), (2, 2), (3, 5), (5, 3), (9, 11)), range(4), (False, True)):
        height, width = shape
        meta = {"row_stride_samples": width + 5, "pattern": pattern, "cfa_phase_x": 1, "cfa_phase_y": 1,
                "active_x": 3, "active_y": 2, "active_width": width, "active_height": height,
                "black_levels": (4000, 7000, 3000, 8000), "white_levels": (19000, 23000, 16000, 27000)}
        sensor_width, sensor_height = width + 5, height + 4
        meta["row_stride_samples"] = sensor_width + 3
        samples = [65535] * (meta["row_stride_samples"] * sensor_height)
        for y in range(height):
            for x in range(width):
                site, _ = h.site_channel(meta, x + 3, y + 2)
                samples[(y + 2) * meta["row_stride_samples"] + x + 3] = (meta["black_levels"][site] +
                    (meta["white_levels"][site] - meta["black_levels"][site]) * 3 // 8) if constant else (x * 7919 + y * 5987 + x * y * 677) % 41000
        fixtures.append((f"small/{shape}/{pattern}/{constant}", (samples, sensor_width, sensor_height, meta)))
    # Exercise internal 256-pixel blocks, both axes, odd dimensions and a tail.
    width, height, stride = 274, 267, 279
    meta = {"row_stride_samples": stride, "pattern": 3, "cfa_phase_x": 1, "cfa_phase_y": 0,
            "active_x": 3, "active_y": 2, "active_width": 269, "active_height": 263,
            "black_levels": (4000, 7000, 3000, 8000), "white_levels": (19000, 23000, 16000, 27000)}
    samples = [65535] * (stride * height)
    for y in range(2, 265):
        for x in range(3, 272):
            samples[y * stride + x] = (x * 7919 + y * 5987 + x * y * 677) % 41000
    fixtures.append(("internal-block-seams", (samples, width, height, meta)))

    binary = bytearray(b"LRMENON1" + struct.pack("<I", len(fixtures)))
    records = []
    for name, (samples, width, height, meta) in fixtures:
        ref = scalar.BayerReference(samples, width, height, meta)
        ox, oy, aw, ah = ref.bounds
        raw = np.asarray([[ref.observed[ox + x, oy + y] for x in range(aw)] for y in range(ah)], dtype=np.float32)
        colors = np.asarray([[ref.colors[ox + x, oy + y] for x in range(aw)] for y in range(ah)])
        thin = aw == 1 or ah == 1
        pattern = "".join("RGB"[h.site_channel(meta, ox + x, oy + y)[1]] for y in range(2) for x in range(2))
        expected = photo.bilinear(np, raw, colors) if thin else oracle(raw, pattern, False).astype(np.float32)
        if not np.isfinite(expected).all():
            raise ValueError("nonfinite reference")
        header = (width, height, meta["row_stride_samples"], meta["pattern"], meta["cfa_phase_x"], meta["cfa_phase_y"], ox, oy, aw, ah)
        binary.extend(struct.pack("<10I8HI", *header, *meta["black_levels"], *meta["white_levels"], len(samples)))
        binary.extend(h.little_bytes("H", samples)); binary.extend(expected.astype("<f4").tobytes())
        records.append({"name": name, "sensor_width_height": [width, height], "metadata": meta, "thin_fallback": thin,
                        "samples_sha256_u16le": h.digest(h.little_bytes("H", samples)),
                        "expected_sha256_f32le": h.digest(expected.astype("<f4").tobytes()), "channel_count": int(expected.size)})
    return bytes(binary), {"reference_version": 1, "source_commit": upstream.COMMIT, "fixture_count": len(records),
                           "upstream_files_sha256": upstream.FILES, "dependencies": upstream.crosscheck.VERSIONS,
                           "colour_array_helper_sha256": helper, "records": records,
                           "generator_sha256_lf": scalar.source_digest(sys.modules[__name__]),
                           "helper_sha256_lf": {Path(m.__file__).name: scalar.source_digest(m) for m in (upstream, scalar, photo, h, upstream.crosscheck)},
                           "binary_format": "LRMENON1; count:u32; per record 10*u32 metadata, 8*u16 levels, sample_count:u32, uint16 sensor rows, float32 RGB active plane; all little endian"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "runtime", "output", "index"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    binary, report = generate(args.source, args.runtime)
    report["binary_sha256"] = h.digest(binary)
    report["research_file"] = args.output.name
    if args.output.exists() and args.output.read_bytes() != binary:
        raise ValueError("refusing to replace different reference data")
    args.output.write_bytes(binary)
    scalar.write_report(args.index, report)
    print(json.dumps({"fixtures": report["fixture_count"], "bytes": len(binary), "sha256": report["binary_sha256"]}))


if __name__ == "__main__":
    main()
