"""Validate camera-corpus declarations and local asset evidence, without decoding.

This checks recorded provenance, hashes, sensor layout and proposed coverage.
It cannot establish rights, patent clearance, camera accuracy or image quality.
Paths stay within the manifest directory; proprietary assets can remain local.
"""
from __future__ import annotations
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys

CONTENT_TAGS = {"neutral_color", "skin", "fine_texture", "slanted_edges", "repeating_detail",
                "deep_shadows", "highlight_headroom", "clipped_highlights"}
ISO_BANDS = {"low", "high"}
ILLUMINANTS = {"daylight", "tungsten"}


def fields(value, required):
    if not isinstance(value, dict) or set(value) != set(required.split()):
        raise ValueError("missing/unknown corpus fields")


def integer(value, name, minimum=0, maximum=0xffffffff):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"invalid {name}")
    return value


def string(value, name):
    if not isinstance(value, str) or not value.strip() or "\0" in value:
        raise ValueError(f"invalid {name}")
    return value


def positive(value, name):
    if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
        raise ValueError(f"invalid {name}")


def tags(value, allowed, name):
    if not isinstance(value, list) or not value or any(not isinstance(v, str) for v in value):
        raise ValueError(f"invalid {name}")
    if len(set(value)) != len(value) or not set(value) <= allowed:
        raise ValueError(f"invalid {name}")
    return set(value)


def checked_file(root, record, expected_size=None):
    fields(record, "path sha256")
    path = string(record["path"], "asset path")
    parts = PurePosixPath(path)
    if parts.is_absolute() or "\\" in path or ":" in path or ".." in parts.parts:
        raise ValueError("asset path must be a confined relative POSIX path")
    target = (root / path).resolve()
    if not target.is_relative_to(root) or not target.is_file():
        raise ValueError("asset missing or outside manifest directory")
    checksum = record["sha256"]
    if not isinstance(checksum, str) or not re.fullmatch("[0-9a-f]{64}", checksum):
        raise ValueError("invalid SHA-256")
    if expected_size is not None and target.stat().st_size != expected_size:
        raise ValueError("decoded Bayer byte count differs from sensor layout")
    digest = hashlib.sha256()
    with target.open("rb") as data:
        for chunk in iter(lambda: data.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != checksum:
        raise ValueError("asset SHA-256 differs from manifest")


def sensor_metadata(value):
    fields(value, "width height row_stride_samples pattern cfa_phase_x cfa_phase_y active_area black_levels white_levels")
    width = integer(value["width"], "width", 1)
    height = integer(value["height"], "height", 1)
    stride = integer(value["row_stride_samples"], "stride", width)
    integer(value["pattern"], "Bayer pattern", maximum=3)
    integer(value["cfa_phase_x"], "CFA phase", maximum=1)
    integer(value["cfa_phase_y"], "CFA phase", maximum=1)
    area = value["active_area"]
    if not isinstance(area, list) or len(area) != 4:
        raise ValueError("invalid active area")
    x, y = (integer(v, "active origin") for v in area[:2])
    w, h = (integer(v, "active size", 1) for v in area[2:])
    if x + w > width or y + h > height:
        raise ValueError("active area outside sensor")
    for name in ("black_levels", "white_levels"):
        levels = value[name]
        if not isinstance(levels, list) or len(levels) != 4:
            raise ValueError("four site levels required")
        for v in levels:
            integer(v, name, maximum=65535)
    if any(b >= w for b, w in zip(value["black_levels"], value["white_levels"])):
        raise ValueError("white must exceed black at each site")
    return stride * height * 2


def validate(record, directory):
    fields(record, "corpus_schema_version purpose coverage_targets assets")
    if type(record["corpus_schema_version"]) is not int or record["corpus_schema_version"] != 1:
        raise ValueError("unsupported camera corpus version")
    string(record["purpose"], "purpose")
    target = record["coverage_targets"]
    fields(target, "minimum_camera_models iso_bands illuminants content_tags")
    minimum = integer(target["minimum_camera_models"], "camera target", 1, 64)
    iso_bands = tags(target["iso_bands"], ISO_BANDS, "ISO targets")
    illuminants = tags(target["illuminants"], ILLUMINANTS, "illuminant targets")
    content_targets = tags(target["content_tags"], CONTENT_TAGS, "content targets")
    assets = record["assets"]
    if not isinstance(assets, list) or len(assets) > 10000:
        raise ValueError("invalid assets")
    root = Path(directory).resolve()
    ids, originals, cameras, conditions, content = set(), set(), set(), set(), set()
    local_only = []
    for asset in assets:
        fields(asset, "id original decoded encoding metadata capture decode_tool rights content_tags rois")
        identity = string(asset["id"], "asset ID")
        if identity in ids:
            raise ValueError("duplicate asset ID")
        ids.add(identity)
        checked_file(root, asset["original"])
        if asset["original"]["sha256"] in originals:
            raise ValueError("duplicate original capture cannot count as new coverage")
        originals.add(asset["original"]["sha256"])
        if asset["encoding"] != "little-endian uint16 Bayer; complete padded sensor buffer":
            raise ValueError("unsupported decoded Bayer encoding")
        checked_file(root, asset["decoded"], sensor_metadata(asset["metadata"]))
        capture = asset["capture"]
        fields(capture, "make model iso iso_band illuminant exposure_seconds wb_reference")
        camera = (string(capture["make"], "make"), string(capture["model"], "model"))
        iso = integer(capture["iso"], "ISO", 1)
        band = capture["iso_band"]
        if (band not in ISO_BANDS or band == "low" and iso > 400 or band == "high" and iso < 1600):
            raise ValueError("ISO does not fit declared low<=400/high>=1600 band")
        if capture["illuminant"] not in ILLUMINANTS:
            raise ValueError("unsupported illuminant target")
        positive(capture["exposure_seconds"], "exposure time")
        wb = capture["wb_reference"]
        fields(wb, "method camera_rgb_gains")
        if wb["method"] not in ("camera-as-shot", "measured-neutral"):
            raise ValueError("unsupported WB reference")
        if not isinstance(wb["camera_rgb_gains"], list) or len(wb["camera_rgb_gains"]) != 3:
            raise ValueError("three WB gains required")
        for gain in wb["camera_rgb_gains"]:
            positive(gain, "WB gain")
        tool = asset["decode_tool"]
        fields(tool, "name version settings")
        for value in tool.values():
            string(value, "decode provenance")
        rights = asset["rights"]
        fields(rights, "review_status identifier redistribution evidence")
        if rights["review_status"] != "reviewed" or rights["redistribution"] not in ("allowed", "local-only"):
            raise ValueError("unreviewed/unknown asset rights declaration")
        string(rights["identifier"], "rights identifier")
        checked_file(root, rights["evidence"])
        if rights["redistribution"] == "local-only":
            local_only.append(identity)
        content.update(tags(asset["content_tags"], CONTENT_TAGS, "content tags"))
        if not isinstance(asset["rois"], list) or not asset["rois"]:
            raise ValueError("sensor-coordinate inspection ROIs required")
        ax, ay, aw, ah = asset["metadata"]["active_area"]
        for roi in asset["rois"]:
            if not isinstance(roi, list) or len(roi) != 4:
                raise ValueError("invalid ROI")
            x, y = (integer(v, "ROI origin") for v in roi[:2])
            w, h = (integer(v, "ROI size", 1) for v in roi[2:])
            if x < ax or y < ay or x + w > ax + aw or y + h > ay + ah:
                raise ValueError("ROI outside active area")
        cameras.add(camera)
        conditions.add((camera, band, capture["illuminant"]))
    missing = [(camera, iso, light) for camera, iso, light in
               itertools.product(sorted(cameras), sorted(iso_bands), sorted(illuminants))
               if (camera, iso, light) not in conditions]
    missing_tags = sorted(content_targets - content)
    return {"corpus_validation_version": 1, "asset_count": len(assets), "camera_model_count": len(cameras),
            "verified_file_records": len(assets) * 3, "missing_camera_models": max(0, minimum - len(cameras)),
            "missing_condition_cells": missing, "missing_content_tags": missing_tags,
            "coverage_ready": len(cameras) >= minimum and not missing and not missing_tags,
            "local_only_assets": sorted(local_only),
            "all_assets_declared_redistributable": bool(assets) and not local_only,
            "limitations": "Hash/layout/declaration checks only; rights and camera correctness are not independently established. Real captures have no analytical RGB ground truth."}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--require-coverage", action="store_true")
    args = parser.parse_args()
    try:
        if args.manifest.stat().st_size > 8 * 1024 * 1024:
            raise ValueError("corpus manifest exceeds 8 MiB")
        record = json.loads(args.manifest.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
        result = validate(record, args.manifest.parent)
    except (ValueError, TypeError, OSError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
    print(f"{result['asset_count']} verified records; coverage_ready={result['coverage_ready']}")
    return 1 if args.require_coverage and not result["coverage_ready"] else 0


if __name__ == "__main__":
    sys.exit(main())
