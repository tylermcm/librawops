"""Optional local research extraction; never part of the engine or its install.

Initial adapter supports Nikon NEF containers. Requires an independently
installed rawpy/NumPy/Pillow runtime. Unpacks sensor
codes without postprocess, WB, demosaic, scaling or display transforms. Camera
originals and derived assets belong in an ignored, local directory.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path

PATTERNS = {"RGGB": 0, "BGGR": 1, "GRBG": 2, "GBRG": 3}


def sha256_file(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def sensor_metadata(raw):
    """Map decoder channel indices to engine pattern-relative site levels."""
    import numpy as np
    plane = raw.raw_image
    pattern = raw.raw_pattern
    if plane.ndim != 2 or plane.dtype != np.uint16 or pattern is None or pattern.shape != (2, 2):
        raise ValueError("only a flat uint16 2x2 Bayer sensor is supported")
    channels = [int(v) for v in pattern.flat]
    desc = raw.color_desc.decode("ascii").rstrip("\0")
    if any(c >= len(desc) for c in channels):
        raise ValueError("invalid CFA channel description")
    name = "".join(desc[c] for c in channels)
    if name not in PATTERNS or raw.num_colors != 3:
        raise ValueError("only three-color RGB Bayer sensors are supported")
    for y in range(4):
        for x in range(4):
            if raw.raw_color(y, x) != channels[(y % 2) * 2 + x % 2]:
                raise ValueError("decoder pattern is not periodic in sensor coordinates")
    black = [int(raw.black_level_per_channel[c]) for c in channels]
    camera_white = raw.camera_white_level_per_channel
    white = [int(camera_white[c] if camera_white is not None else raw.white_level) for c in channels]
    if any(not 0 <= b < w <= 65535 for b, w in zip(black, white)):
        raise ValueError("site levels cannot be represented by the engine")
    s = raw.sizes
    height, width = plane.shape
    if (width, height) != (s.raw_width, s.raw_height):
        raise ValueError("unpacked plane and sensor dimensions disagree")
    active = [int(s.left_margin), int(s.top_margin), int(s.width), int(s.height)]
    x, y, w, h = active
    if min(x, y) < 0 or min(w, h) < 1 or x + w > width or y + h > height:
        raise ValueError("invalid decoder visible rectangle")
    return {"width": width, "height": height, "row_stride_samples": width,
            "pattern": PATTERNS[name], "cfa_phase_x": 0, "cfa_phase_y": 0,
            "active_area": active, "black_levels": black, "white_levels": white}, name, channels


def raw_paths(folder, only=None):
    folder = folder.resolve()
    paths = sorted(p for p in folder.iterdir() if p.is_file() and p.suffix.lower() == ".nef")
    if only is not None:
        if (not only or any(not isinstance(name, str) or Path(name).name != name
                            or "/" in name or "\\" in name for name in only)
                or len(set(only)) != len(only)):
            raise ValueError("--only requires unique local NEF filenames")
        by_name = {p.name: p for p in paths}
        if any(name not in by_name for name in only):
            raise ValueError("selected NEF file is missing or unsupported")
        paths = sorted(by_name[name] for name in only)
    if not paths:
        raise ValueError("no supported Nikon NEF files found")
    for path in paths:
        path.resolve().relative_to(folder)
    return paths


def extract(folder, output, inventory_only=False, only=None):
    import rawpy
    import numpy as np
    from PIL import Image
    folder, output = folder.resolve(), output.resolve()
    output.relative_to(folder)  # All local assets remain within one confined root.
    output.mkdir(parents=True, exist_ok=True)
    records = []
    paths = raw_paths(folder, only)
    for path in paths:
        with Image.open(path) as image:
            exif = image.getexif()
            other = exif.get_ifd(34665)
            capture = {"make": str(exif.get(271, "")), "model": str(exif.get(272, "")),
                       "iso": int(other.get(34855, 0)), "exposure_seconds": float(other.get(33434, 0)),
                       "exif_light_source": int(other.get(37384, 0)), "orientation": int(exif.get(274, 1))}
        with rawpy.imread(str(path)) as raw:
            metadata, name, channels = sensor_metadata(raw)
            wb = [float(v) for v in raw.camera_whitebalance]
            record = {"id": path.stem, "original": {"path": path.name, "sha256": sha256_file(path)},
                      "metadata": metadata, "capture": capture, "pattern_name": name,
                      "decoder_channel_indices": channels, "camera_whitebalance": wb,
                      "decoder_white_level": int(raw.white_level),
                      "white_level_selection": "camera_white_level_per_channel" if raw.camera_white_level_per_channel is not None else "white_level",
                      "decoder_rgb_xyz_matrix_uninterpreted": raw.rgb_xyz_matrix.tolist(),
                      "decoder_recommended_crop": [int(getattr(raw.sizes, k, 0)) for k in
                           ("crop_left_margin", "crop_top_margin", "crop_width", "crop_height")],
                      "admission": "local study only; illuminant/content/profile review pending; no redistribution"}
            thumb = raw.extract_thumb()
            if thumb.format == rawpy.ThumbFormat.JPEG:
                preview = Image.open(io.BytesIO(thumb.data)).convert("RGB")
            else:
                preview = Image.fromarray(thumb.data)
            # Embedded JPEG is an appearance reference, never RGB ground truth.
            if capture["orientation"] in (3, 6, 8):
                preview = preview.rotate({3: 180, 6: 270, 8: 90}[capture["orientation"]], expand=True)
            preview.thumbnail((480, 360))
            thumb_path = output / (path.stem + "-camera-thumb.png")
            preview.save(thumb_path)
            record["camera_thumbnail"] = {"path": thumb_path.relative_to(folder).as_posix(), "sha256": sha256_file(thumb_path)}
            if not inventory_only:
                decoded = output / (path.stem + "-bayer-u16le.bin")
                codes = np.ascontiguousarray(raw.raw_image, dtype="<u2")
                code_hash = hashlib.sha256(memoryview(codes).cast("B")).hexdigest()
                if decoded.exists():
                    if sha256_file(decoded) != code_hash:
                        raise ValueError("refusing to overwrite differing decoded evidence")
                else:
                    codes.tofile(decoded)
                record["decoded"] = {"path": decoded.relative_to(folder).as_posix(), "sha256": code_hash,
                                     "bytes": codes.nbytes}
            records.append(record)
            print(f"{path.name}: {capture['model']}, ISO {capture['iso']}, {metadata['width']}x{metadata['height']}, {name}", flush=True)
    report = {"local_extraction_schema_version": 1, "decoder": {"rawpy": rawpy.__version__, "libraw": list(rawpy.libraw_version)},
              "settings": "imread/open+unpack only; no postprocess, WB, demosaic, scaling or user corrections; contiguous u16le storage; decoder-visible active rectangle",
              "decoder_distribution_admitted": False, "assets": records}
    (output / "extraction-v1.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--inventory-only", action="store_true")
    parser.add_argument("--only", nargs="+", help="Extract just these local NEF filenames into a new study directory")
    args = parser.parse_args()
    extract(args.folder, args.output, args.inventory_only, args.only)
