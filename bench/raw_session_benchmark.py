"""Local decoded-camera latency/cache/process-memory benchmark (optional NumPy).

No decoding, PNG output, denoise or display is timed. Use fresh processes/new
report paths for before/after comparisons; peak memory is process-wide, not an
allocator trace or an isolated per-render peak.
"""
import argparse
import ctypes
import gc
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from raw_camera_extract import sha256_file
from raw_camera_engine_check import camera_matrix, native_metadata


def memory():
    if os.name != "nt": return {"available": False}
    from ctypes import wintypes
    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    fn = psapi.GetProcessMemoryInfo
    fn.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    fn.restype = wintypes.BOOL
    counter = Counters(); counter.cb = ctypes.sizeof(counter)
    if not fn(wintypes.HANDLE(-1), ctypes.byref(counter), counter.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return {"available": True, "working_set_bytes": counter.WorkingSetSize,
            "peak_working_set_bytes": counter.PeakWorkingSetSize,
            "private_bytes": counter.PrivateUsage, "peak_commit_bytes": counter.PeakPagefileUsage}


def cpu_seconds():
    return time.process_time()  # Sum of CPU time across the process's threads.


def stage_manifest(document, output):
    """Keep the requested output and its upstream closure, independent of ID order."""
    operations = {op["id"]: op for op in document["operations"]}
    sources = {source["id"] for source in document["sources"]}
    needed, visiting = set(), set()
    def visit(node):
        if node in sources or node in needed: return
        if node in visiting: raise ValueError("cycle in benchmark graph")
        if node not in operations: raise ValueError("missing benchmark graph input")
        visiting.add(node)
        for upstream in operations[node]["inputs"].values(): visit(upstream)
        visiting.remove(node); needed.add(node)
    visit(output)
    return {**document, "operations": [op for op in document["operations"] if op["id"] in needed], "output": output}


def run(args):
    import numpy as np
    sys.path.insert(0, str(args.module_dir.resolve()))
    import rawengine_native as raw
    if args.output.exists(): raise ValueError("use a new report path; prior measurements are retained")
    doc = json.loads(args.extraction.read_text(encoding="utf-8"))
    record = next(r for r in doc["assets"] if r["id"] == args.capture)
    meta = record["metadata"]
    decoded = args.folder / record["decoded"]["path"]
    decoded.resolve().relative_to(args.folder.resolve())
    if sha256_file(decoded) != record["decoded"]["sha256"]: raise ValueError("decoded hash mismatch")
    codes = np.memmap(decoded, dtype="<u2", mode="r", shape=(meta["height"], meta["row_stride_samples"]))
    if codes.nbytes != decoded.stat().st_size: raise ValueError("sensor byte count mismatch")
    wb = record["camera_whitebalance"]
    recipe = dict(zip(("red_gain", "green_gain", "blue_gain"), (wb[i] / wb[1] for i in range(3))),
                  exposure_stops=0.0, camera_to_xyz_d50=camera_matrix(record), working_space="prophoto-d50",
                  output_mode="srgb-preview")
    before_init = memory(); started = time.perf_counter()
    session = raw.RawSession(codes, meta["width"], meta["height"], native_metadata(meta),
                            cache_bytes=args.cache_mib * 1024 * 1024, workers=1,
                            demosaic={"algorithm": "rawengine.menon_base", "processing_version": 1})
    initialization_ms = (time.perf_counter() - started) * 1000
    cases = {}
    def measure(name, action, clear=False):
        if clear: session.clear_cache()
        gc.collect()
        initial = session.cache_stats(); mem = memory(); cpu = cpu_seconds(); wall = time.perf_counter()
        result = action()
        elapsed = (time.perf_counter() - wall) * 1000; used_cpu = (cpu_seconds() - cpu) * 1000
        final = session.cache_stats()
        row = {"wall_ms": elapsed, "cpu_ms": used_cpu, "output_dimensions": list(result[:2]),
               "output_bytes": len(result[2]), "sha256_f32": hashlib.sha256(result[2]).hexdigest(),
               "hits_added": final["hits"] - initial["hits"], "misses_added": final["misses"] - initial["misses"],
               "cache": final, "memory_before": mem, "memory_after": memory()}
        cases.setdefault(name, []).append(row)
        print(f"{name}: {elapsed:.3f} ms, CPU {used_cpu:.3f} ms, hits {row['hits_added']}, misses {row['misses_added']}", flush=True)
        return row
    try:
        native = json.loads(session.export_manifest(recipe))
        ids = {op["type"]: op["id"] for op in native["operations"]}
        stages = []
        for name, output in (("demosaic_native", native["sources"][0]["id"]),
                             ("wb_exposure_native", ids["rawengine.exposure"]),
                             ("calibrated_native", ids["rawengine.camera_to_working"]),
                             ("encoded_native", native["output"])):
            stage = stage_manifest(native, output)
            stages.append((name, json.dumps(stage)))
        ax, ay, aw, ah = meta["active_area"]
        if min(aw, ah) < 2048: raise ValueError("benchmark requires at least a 2048-square active plane")
        native_roi = {"x": ax + 1024, "y": ay + 1024, "roi_width": 1024, "roi_height": 1024, "tile_size": 1024}
        if not args.skip_stages:
            for name, text in stages:
                for _ in range(args.repeats): measure(name, lambda: session.render_manifest(text, native_roi), clear=True)
        full = {**recipe, "mip": 2, "quality": "preview", "tile_size": args.tile_size}
        late = {**full, "tone_shoulder": .8}
        for _ in range(args.repeats):
            cold = measure("full_cold", lambda: session.render(full), clear=True)
            warm = measure("full_warm", lambda: session.render(full))
            if cold["sha256_f32"] != warm["sha256_f32"]: raise ValueError("cold/warm pixel parity failed")
            edit = measure("full_late_tone", lambda: session.render(late))
            # Compare cached edit with its own cache-cleared reference once.
        uncached_edit = measure("full_late_reference", lambda: session.render(late), clear=True)
        if edit["sha256_f32"] != uncached_edit["sha256_f32"]: raise ValueError("cached edit differs from cold reference")
        viewport = {**full, "x": 256, "y": 256, "roi_width": 256, "roi_height": 256}
        for _ in range(args.repeats):
            cold = measure("viewport_cold", lambda: session.render(viewport), clear=True)
            warm = measure("viewport_warm", lambda: session.render(viewport))
            if cold["sha256_f32"] != warm["sha256_f32"]: raise ValueError("viewport cache parity failed")
            changed = measure("viewport_late_tone", lambda: session.render({**viewport, "tone_shoulder": .8}))
            measure("viewport_exposure", lambda: session.render({**viewport, "exposure_stops": .25}))
            measure("viewport_wb", lambda: session.render({**viewport, "red_gain": recipe["red_gain"] * 1.05}))
        report = {"raw_session_benchmark_version": 1, "capture": args.capture, "width": meta["width"], "height": meta["height"],
                  "decoded_sha256": record["decoded"]["sha256"], "extraction_sha256": sha256_file(args.extraction),
                  "native_module_sha256": sha256_file(Path(raw.__file__)),
                  "engine_dll_sha256": sha256_file(args.module_dir / "RawEngine.dll"),
                  "menon_source_sha256_lf": hashlib.sha256((args.module_dir / "MenonDemosaic.cpp" if (args.module_dir / "MenonDemosaic.cpp").is_file() else ROOT / "MenonDemosaic.cpp").read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
                  "benchmark_source_sha256_lf": hashlib.sha256(Path(__file__).read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
                  "python": platform.python_version(), "platform": platform.platform(),
                  "environment": {k: os.environ.get(k) for k in ("OMP_NUM_THREADS", "OMP_DYNAMIC", "OMP_NESTED")},
                  "cache_mib": args.cache_mib, "tile_size": args.tile_size, "repeats": args.repeats,
                  "session_initialization_ms": initialization_ms, "memory_before_init": before_init,
                  "memory_final": memory(), "cases": cases,
                  "median_ms": {k: statistics.median(r["wall_ms"] for r in rows) for k, rows in cases.items()},
                  "scope": "Decoded local sensor, one session worker, as-shot WB/diagnostic matrix. CPU time sums threads. Initialization includes ownership copy/validation/fingerprint. Render includes graph/cache and output byte copy; no decode/export/display. Memory is process current/peak working set and private commit, includes Python/NumPy/mapped source/cache/output. No allocator trace; stage-time differences include intermediate cache storage/copy and are not exclusive CPU samples."}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print(json.dumps({"median_ms": report["median_ms"], "peak_memory": report["memory_final"]}), flush=True)
    finally: session.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("module_dir", "folder", "extraction", "output"): parser.add_argument(name, type=Path)
    parser.add_argument("--capture", required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--cache-mib", type=int, default=64)
    parser.add_argument("--tile-size", type=int, default=256)
    parser.add_argument("--skip-stages", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10 or not 0 <= args.cache_mib <= 2048 or not 1 <= args.tile_size <= 2048:
        parser.error("benchmark budget outside permitted bounds")
    run(args)
