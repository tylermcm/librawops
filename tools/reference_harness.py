"""Deterministic Photoshop raster fixtures and strict 16-bit TIFF comparison.

Standard-library only. This is a test/reference tool, not a product file codec.
It intentionally rejects compressed, tiled, planar, alpha, and oriented TIFFs.
"""

from __future__ import annotations

import argparse
import array
import colorsys
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from typing import Callable, Iterator


PROFILE = Path(__file__).resolve().parents[1] / "tests/reference/profiles/sRGB2014.icc"
PROFILE_SHA256 = "384b832de3412066743b52a75ee906b6fb9fb8d9e09e936fc2c43223815c6e0a"
TIFF_TAGS = (256, 257, 258, 259, 262, 273, 274, 277, 278, 279, 284, 339, 34675)
METADATA_FIELDS = (
    "application", "application_build", "source_sha256", "reference_sha256",
    "process_version", "document_profile_sha256", "output_profile_sha256",
    "output_bit_depth", "compression", "pixel_order", "byte_order",
    "orientation", "baseline_or_adjustment", "all_adjustment_settings",
    "automatic_adjustments", "color_settings", "profile_policy",
    "rendering_intent", "black_point_compensation", "resize",
    "output_sharpening", "export_method",
)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def checked_profile() -> bytes:
    profile = PROFILE.read_bytes()
    if digest(profile) != PROFILE_SHA256 or profile[36:40] != b"acsp":
        raise ValueError("pinned ICC fixture profile has changed")
    return profile


def write_if_unchanged(path: Path, content: bytes) -> None:
    if path.exists():
        if path.read_bytes() != content:
            raise FileExistsError(f"existing fixture differs: {path}")
        return
    path.write_bytes(content)


def make_tiff_header(width: int, height: int, profile: bytes,
                     endian: str = "<") -> bytes:
    if width <= 0 or height <= 0 or width * height * 6 > 0xFFFFFFFF:
        raise ValueError("TIFF fixture dimensions exceed classic TIFF limits")
    if endian not in ("<", ">"):
        raise ValueError("TIFF byte order must be little or big endian")
    # Classic TIFF. One interleaved uncompressed RGB strip.
    count = len(TIFF_TAGS)
    ifd_end = 8 + 2 + count * 12 + 4
    bits_offset = ifd_end
    sample_format_offset = bits_offset + 6
    profile_offset = sample_format_offset + 6
    pixels_offset = profile_offset + len(profile)
    if pixels_offset % 2:
        pixels_offset += 1
    if pixels_offset + width * height * 6 > 0xFFFFFFFF:
        raise ValueError("TIFF fixture offset exceeds classic TIFF limits")
    values = {
        256: (4, 1, width), 257: (4, 1, height),
        258: (3, 3, bits_offset), 259: (3, 1, 1),
        262: (3, 1, 2), 273: (4, 1, pixels_offset),
        274: (3, 1, 1), 277: (3, 1, 3),
        278: (4, 1, height), 279: (4, 1, width * height * 6),
        284: (3, 1, 1), 339: (3, 3, sample_format_offset),
        34675: (7, len(profile), profile_offset),
    }
    out = bytearray((b"II" if endian == "<" else b"MM") +
                    struct.pack(endian + "HIH", 42, 8, count))
    for tag in TIFF_TAGS:
        field_type, field_count, value = values[tag]
        inline_value = value << 16 if endian == ">" and field_type == 3 and field_count == 1 else value
        out += struct.pack(endian + "HHII", tag, field_type, field_count, inline_value)
    out += struct.pack(endian + "I", 0)
    out += struct.pack(endian + "3H", 16, 16, 16)
    out += struct.pack(endian + "3H", 1, 1, 1)
    out += profile
    out += b"\0" * (pixels_offset - len(out))
    return bytes(out)


def write_tiff(path: Path, width: int, height: int,
               rows: Iterator[bytes], profile: bytes) -> None:
    header = make_tiff_header(width, height, profile)
    if path.exists():
        # Keep reruns idempotent and refuse to overwrite changed captures.
        expected = hashlib.sha256()
        expected.update(header)
        count = 0
        for row in rows:
            if len(row) != width * 6:
                raise ValueError("fixture row has incorrect byte count")
            expected.update(row)
            count += 1
        if count != height:
            raise ValueError("fixture row count is incorrect")
        if file_digest(path) != expected.hexdigest():
            raise FileExistsError(f"existing fixture differs: {path}")
        return
    with path.open("wb") as output:
        output.write(header)
        count = 0
        for row in rows:
            if len(row) != width * 6:
                raise ValueError("fixture row has incorrect byte count")
            output.write(row)
            count += 1
        if count != height:
            raise ValueError("fixture row count is incorrect")


def pack_pixels(pixels: Iterator[tuple[int, int, int]]) -> bytes:
    data = array.array("H")
    for pixel in pixels:
        data.extend(pixel)
    if sys.byteorder != "little":
        data.byteswap()
    return data.tobytes()


def neutral_rows() -> Iterator[bytes]:
    for y in range(64):
        def pixels() -> Iterator[tuple[int, int, int]]:
            for x in range(256):
                if y < 16:
                    v = x * 257
                elif y < 32:
                    v = x  # near-black 16-bit codes
                elif y < 48:
                    v = 65535 - x  # near-white 16-bit codes
                else:
                    v = (x // 16) * 4369
                yield (v, v, v)
        yield pack_pixels(pixels())


def color_rows() -> Iterator[bytes]:
    for y in range(192):
        band = y // 32
        saturation = (0.25, 0.5, 1.0)[band % 3]
        value = (0.5, 1.0)[band // 3]
        def pixels() -> Iterator[tuple[int, int, int]]:
            for x in range(192):
                hue = (x // 32) / 6.0
                rgb = colorsys.hsv_to_rgb(hue, saturation, value)
                yield tuple(round(component * 65535) for component in rgb)
        yield pack_pixels(pixels())


def edge_rows() -> Iterator[bytes]:
    for y in range(128):
        def pixels() -> Iterator[tuple[int, int, int]]:
            for x in range(256):
                if x < 128:
                    v = 4096 if x < 64 else 57344
                    yield (v, v, v)
                else:
                    v = 65535 if ((x + y) & 1) else 0
                    yield (v, 65535 - v, v)
        yield pack_pixels(pixels())


FIXTURES: tuple[tuple[str, int, int, Callable[[], Iterator[bytes]], str], ...] = (
    ("neutral_ramps", 256, 64, neutral_rows, "tone, shadows, highlights, neutral response"),
    ("color_grid", 192, 192, color_rows, "hue and saturation response"),
    ("edges_checker", 256, 128, edge_rows, "edges, channel fringes, fine detail"),
)


class Tiff16:
    def __init__(self, path: Path):
        self.path = path
        self.file = path.open("rb")
        try:
            self.size = path.stat().st_size
            marker = self._read(0, 8)
            self.endian = "<" if marker[:2] == b"II" else ">" if marker[:2] == b"MM" else ""
            if not self.endian or struct.unpack(self.endian + "H", marker[2:4])[0] != 42:
                raise ValueError("expected classic TIFF, not BigTIFF or another format")
            ifd = struct.unpack(self.endian + "I", marker[4:8])[0]
            self.tags = self._tags(ifd)
            self.width = self._scalar(256)
            self.height = self._scalar(257)
            if self.width <= 0 or self.height <= 0:
                raise ValueError("invalid TIFF dimensions")
            if self._values(258) != [16, 16, 16]:
                raise ValueError("comparison requires 16-bit RGB")
            requirements = {259: 1, 262: 2, 274: 1, 277: 3, 284: 1}
            for tag, expected in requirements.items():
                if self._scalar(tag, expected if tag in (274, 284) else None) != expected:
                    raise ValueError(f"unsupported TIFF tag {tag}; require uncompressed interleaved upright RGB")
            if self._values(339, [1, 1, 1]) != [1, 1, 1]:
                raise ValueError("comparison requires unsigned integer samples")
            if 338 in self.tags or 322 in self.tags or 324 in self.tags:
                raise ValueError("alpha or tiled TIFF is unsupported")
            self.rows_per_strip = self._scalar(278)
            if self.rows_per_strip <= 0:
                raise ValueError("TIFF rows-per-strip must be positive")
            self.strip_offsets = self._values(273)
            self.strip_sizes = self._values(279)
            strips = (self.height + self.rows_per_strip - 1) // self.rows_per_strip
            if len(self.strip_offsets) != strips or len(self.strip_sizes) != strips:
                raise ValueError("TIFF strip layout is invalid")
            for index, (offset, size) in enumerate(zip(self.strip_offsets, self.strip_sizes)):
                rows = min(self.rows_per_strip, self.height - index * self.rows_per_strip)
                if size < rows * self.width * 6 or offset + size > self.size:
                    raise ValueError("TIFF strip is truncated")
            self.profile = self._raw(34675) if 34675 in self.tags else b""
        except Exception:
            self.file.close()
            raise

    def _read(self, offset: int, count: int) -> bytes:
        if offset < 0 or count < 0 or offset + count > self.size:
            raise ValueError("TIFF offset or field exceeds file size")
        self.file.seek(offset)
        data = self.file.read(count)
        if len(data) != count:
            raise ValueError("truncated TIFF data")
        return data

    def _tags(self, offset: int) -> dict[int, tuple[int, int, bytes]]:
        count = struct.unpack(self.endian + "H", self._read(offset, 2))[0]
        if count > 4096:
            raise ValueError("TIFF has too many fields")
        entries: dict[int, tuple[int, int, bytes]] = {}
        for index in range(count):
            data = self._read(offset + 2 + index * 12, 12)
            tag, field_type, length = struct.unpack(self.endian + "HHI", data[:8])
            if tag in entries:
                raise ValueError("duplicate TIFF tag")
            entries[tag] = (field_type, length, data[8:12])
        next_ifd = struct.unpack(self.endian + "I", self._read(offset + 2 + count * 12, 4))[0]
        if next_ifd:
            raise ValueError("multi-page TIFF is unsupported")
        return entries

    def _raw(self, tag: int) -> bytes:
        field_type, count, inline = self.tags[tag]
        unit = {1: 1, 3: 2, 4: 4, 7: 1}.get(field_type)
        if unit is None or count > self.size // unit:
            raise ValueError(f"unsupported or oversized TIFF field {tag}")
        length = count * unit
        offset = struct.unpack(self.endian + "I", inline)[0]
        return inline[:length] if length <= 4 else self._read(offset, length)

    def _values(self, tag: int, default: list[int] | None = None) -> list[int]:
        if tag not in self.tags:
            if default is not None:
                return default
            raise ValueError(f"missing TIFF tag {tag}")
        field_type, count, _ = self.tags[tag]
        if field_type not in (3, 4) or count > 1000000:
            raise ValueError(f"unsupported TIFF numeric field {tag}")
        code = "H" if field_type == 3 else "I"
        return list(struct.unpack(self.endian + str(count) + code, self._raw(tag)))

    def _scalar(self, tag: int, default: int | None = None) -> int:
        values = self._values(tag, [default] if default is not None else None)
        if len(values) != 1:
            raise ValueError(f"TIFF tag {tag} must be scalar")
        return values[0]

    def row(self, y: int) -> array.array:
        if y < 0 or y >= self.height:
            raise ValueError("TIFF row is outside image")
        strip = y // self.rows_per_strip
        offset = self.strip_offsets[strip] + (y % self.rows_per_strip) * self.width * 6
        data = array.array("H")
        data.frombytes(self._read(offset, self.width * 6))
        if (self.endian == "<") != (sys.byteorder == "little"):
            data.byteswap()
        return data

    def close(self) -> None:
        self.file.close()

    def __enter__(self) -> Tiff16:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def generate(directory: Path) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    profile = checked_profile()
    cases = []
    for name, width, height, rows, purpose in FIXTURES:
        path = directory / f"{name}.tif"
        write_tiff(path, width, height, rows(), profile)
        with Tiff16(path) as image:
            if (image.width, image.height, digest(image.profile)) != (width, height, PROFILE_SHA256):
                raise ValueError("generated TIFF failed independent structural read")
        cases.append({"id": name, "file": path.name, "sha256": file_digest(path),
                      "width": width, "height": height, "purpose": purpose,
                      "input_profile_sha256": PROFILE_SHA256,
                      "input_encoding": "16-bit unsigned encoded sRGB",
                      "adobe_result": "Not measured"})
    manifest = {"schema_version": 1, "generator": "tools/reference_harness.py",
                "icc_source": "https://registry.color.org/rgb-registry/profiles/sRGB2014.icc",
                "icc_sha256": PROFILE_SHA256, "profile_terms":
                "ICC profile may be copied, distributed, embedded, made, used and sold without restriction; preserve its identity and copyright tag.",
                "cases": cases, "raw_cases": "Pending licensed RAW corpus and decoder boundary"}
    write_if_unchanged(directory / "manifest.json",
                       (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    template = {field: None for field in METADATA_FIELDS}
    template.update({"schema_version": 1, "source_case_id": None,
                     "source_file": None, "reference_file": None,
                     "status": "Not measured", "notes": None})
    write_if_unchanged(directory / "capture_template.json",
                       (json.dumps(template, indent=2) + "\n").encode("utf-8"))
    return manifest


def validate_capture(path: Path, source: Path, reference: Path,
                     profile_sha256: str, byte_order: str) -> dict:
    capture = json.loads(path.read_text(encoding="utf-8"))
    missing = [field for field in METADATA_FIELDS if capture.get(field) is None]
    if missing:
        raise ValueError("capture metadata is incomplete: " + ", ".join(missing))
    if capture.get("application") not in ("Photoshop", "Adobe Camera Raw"):
        raise ValueError("capture application must be Photoshop or Adobe Camera Raw")
    if capture.get("status") != "Measured":
        raise ValueError("capture status must be Measured")
    expected_layout = {"output_bit_depth": 16, "compression": "none",
                       "pixel_order": "interleaved", "orientation": 1,
                       "byte_order": byte_order}
    for field, expected in expected_layout.items():
        if capture[field] != expected:
            raise ValueError(f"capture {field} disagrees with TIFF layout")
    if not isinstance(capture["all_adjustment_settings"], dict) or \
       not isinstance(capture["automatic_adjustments"], bool):
        raise ValueError("capture adjustment settings and auto flag have wrong types")
    if capture["source_sha256"] != file_digest(source) or \
       capture["reference_sha256"] != file_digest(reference) or \
       capture["output_profile_sha256"] != profile_sha256:
        raise ValueError("capture source, reference, or ICC checksum disagrees")
    return capture


def compare(source: Path, reference: Path, diff_path: Path | None = None,
            capture_path: Path | None = None, capture_source: Path | None = None) -> dict:
    with Tiff16(source) as a, Tiff16(reference) as b:
        if (a.width, a.height) != (b.width, b.height):
            raise ValueError("TIFF dimensions differ; align geometry before color comparison")
        if not a.profile or a.profile != b.profile:
            raise ValueError("embedded ICC bytes must be present and identical")
        captured = None
        if capture_path:
            if not capture_source:
                raise ValueError("--capture-source is required with --capture")
            captured = validate_capture(capture_path, capture_source, reference,
                                        digest(b.profile),
                                        "little" if b.endian == "<" else "big")
        count = a.width * a.height * 3
        abs_sum = square_sum = max_error = nonzero = 0
        def difference_rows() -> Iterator[bytes]:
            nonlocal abs_sum, square_sum, max_error, nonzero
            for y in range(a.height):
                left, right = a.row(y), b.row(y)
                errors = array.array("H")
                for lhs, rhs in zip(left, right):
                    error = abs(lhs - rhs)
                    abs_sum += error
                    square_sum += error * error
                    max_error = max(max_error, error)
                    nonzero += error != 0
                    errors.append(error)
                if sys.byteorder != "little":
                    errors.byteswap()
                yield errors.tobytes()
        if diff_path:
            if diff_path.resolve() in (source.resolve(), reference.resolve()):
                raise ValueError("difference path must not replace an input")
            # Difference RGB is numeric error, not a color image: no ICC tag.
            write_tiff(diff_path, a.width, a.height, difference_rows(), b"")
        else:
            for _ in difference_rows():
                pass
        mse = square_sum / count
        return {"schema_version": 1, "comparison_kind":
                "Adobe reference" if captured else "unverified image pair",
                "source_sha256": file_digest(source),
                "reference_sha256": file_digest(reference),
                "profile_sha256": digest(a.profile), "width": a.width,
                "height": a.height, "channels": 3, "sample_bits": 16,
                "max_abs_code": max_error,
                "mean_abs_code": abs_sum / count,
                "rmse_code": math.sqrt(mse),
                "psnr_db": None if mse == 0 else 20 * math.log10(65535) - 10 * math.log10(mse),
                "different_samples": nonzero,
                "difference_file": str(diff_path) if diff_path else None,
                "capture_file": str(capture_path) if captured else None,
                "adobe_compatibility": "Not assessed"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    generate_command = sub.add_parser("generate", help="create deterministic TIFF fixtures")
    generate_command.add_argument("output", type=Path)
    compare_command = sub.add_parser("compare", help="compare uncompressed 16-bit RGB TIFFs")
    compare_command.add_argument("source", type=Path)
    compare_command.add_argument("reference", type=Path)
    compare_command.add_argument("--diff", type=Path)
    compare_command.add_argument("--capture", type=Path)
    compare_command.add_argument("--capture-source", type=Path)
    compare_command.add_argument("--json", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "generate":
            result = generate(args.output)
        else:
            if args.json and args.json.resolve() in {
                path.resolve() for path in (args.source, args.reference,
                                             args.diff, args.capture,
                                             args.capture_source) if path
            }:
                raise ValueError("JSON output must not replace an input, capture, or difference file")
            result = compare(args.source, args.reference, args.diff,
                             args.capture, args.capture_source)
        output = json.dumps(result, indent=2) + "\n"
        if args.command == "compare" and args.json:
            args.json.write_text(output, encoding="utf-8")
        print(output, end="")
    except (OSError, ValueError) as error:
        parser.exit(2, f"reference harness: {error}\n")


if __name__ == "__main__":
    main()
