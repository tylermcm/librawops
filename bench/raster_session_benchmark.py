"""Synthetic Python raster render timings; no file decode or export included."""
import argparse
import array
import json
import platform
import statistics
import sys
import time


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module_dir", help="directory containing rawengine_native")
    parser.add_argument("--width", type=int, default=2048)
    parser.add_argument("--height", type=int, default=1536)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--mip", type=int, choices=(0, 1, 2), help="default 2; default 0 with resize")
    parser.add_argument("--tile-size", type=int, default=256)
    parser.add_argument("--cache-mib", type=int, default=64)
    parser.add_argument("--crop", type=int, nargs=4, metavar=("X", "Y", "WIDTH", "HEIGHT"))
    parser.add_argument("--resize", type=int, nargs=2, metavar=("WIDTH", "HEIGHT"))
    parser.add_argument("--resize-filter", choices=("nearest", "bilinear", "area"), default="bilinear")
    parser.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0, help="clockwise degrees, after crop")
    parser.add_argument("--flip-horizontal", action="store_true", help="flip after rotation")
    parser.add_argument("--flip-vertical", action="store_true", help="flip after rotation")
    args = parser.parse_args()
    if args.mip is None:
        args.mip = 0 if args.resize else 2
    if min(args.width, args.height, args.repeats, args.tile_size) < 1 or args.cache_mib < 0:
        parser.error("dimensions, repeats and tile size must be positive; cache budget must be nonnegative")
    if args.crop:
        x, y, w, h = args.crop
        if min(x, y) < 0 or min(w, h) < 1 or x + w > args.width or y + h > args.height:
            parser.error("crop must be nonempty and inside the source")
    if args.resize and min(args.resize) < 1:
        parser.error("resize dimensions must be positive")
    sys.path.insert(0, args.module_dir)
    import rawengine_native as raw

    # Fixture creation is excluded from timings. No NumPy or external assets.
    pixels = array.array("f")
    for y in range(args.height):
        row = array.array("f")
        for x in range(args.width):
            row.extend(((x % 1021) / 1021, (y % 761) / 761, ((x + y) % 509) / 509))
        pixels.extend(row)
    begin = time.perf_counter()
    session = raw.RasterSession(pixels, args.width, args.height, "prophoto-d50",
                               cache_bytes=args.cache_mib * 1024 * 1024)
    initialization_ms = (time.perf_counter() - begin) * 1000
    settings = {"mip": args.mip, "quality": "preview" if args.mip else "final", "output_mode": "srgb-preview",
                "tile_size": args.tile_size, "tone_shoulder": 0.8, "tone_gamma": 1.3,
                "rotate": args.rotate, "flip_horizontal": args.flip_horizontal, "flip_vertical": args.flip_vertical}
    if args.crop:
        settings["crop"] = tuple(args.crop)
    if args.resize:
        settings.update(resize=tuple(args.resize), resize_filter=args.resize_filter)
    timings = {key: [] for key in ("one_shot_ms", "session_cold_ms", "session_warm_ms", "late_tone_ms")}
    last = None
    def timed(key, action):
        begin = time.perf_counter()
        result = action()
        timings[key].append((time.perf_counter() - begin) * 1000)
        return result
    for repeat in range(args.repeats):
        one_shot = timed("one_shot_ms", lambda: raw.render_raster(pixels, args.width, args.height,
                         {**settings, "working_space": "prophoto-d50"}))
        session.clear_cache()
        cold = timed("session_cold_ms", lambda: session.render(settings))
        before_warm = session.cache_stats()
        warm = timed("session_warm_ms", lambda: session.render(settings))
        after_warm = session.cache_stats()
        revision = {**settings, "tone_shoulder": 1.0 + repeat * 0.1}
        late = timed("late_tone_ms", lambda: session.render(revision))
        after_late = session.cache_stats()
        if one_shot != cold or cold != warm:
            raise RuntimeError("one-shot/cold/warm pixel parity failed")
        expected = raw.render_raster(pixels, args.width, args.height,
                                    {**revision, "working_space": "prophoto-d50"})
        if late != expected:
            raise RuntimeError("late-tone pixel parity failed")
        last = {"output_width": cold[0], "output_height": cold[1], "output_bytes": len(cold[2]),
                "warm_hits_added": after_warm["hits"] - before_warm["hits"],
                "warm_misses_added": after_warm["misses"] - before_warm["misses"],
                "late_hits_added": after_late["hits"] - after_warm["hits"],
                "late_misses_added": after_late["misses"] - after_warm["misses"]}
    print(json.dumps({"python": platform.python_version(), "platform": platform.platform(),
        "width": args.width, "height": args.height, "source_bytes": len(pixels) * pixels.itemsize,
        "mip": args.mip, "tile_size": args.tile_size, "repeats": args.repeats,
        "crop": args.crop, "resize": args.resize, "resize_filter": args.resize_filter if args.resize else None,
        "rotate": args.rotate, "flip_horizontal": args.flip_horizontal, "flip_vertical": args.flip_vertical,
        "initialization_ms": initialization_ms,
        "median_timings": {key: statistics.median(samples) for key, samples in timings.items()},
        "last_iteration": last, "cache": session.cache_stats(),
        "scope": "Synthetic owned float32 source; initialization includes copy/validation/fingerprint. Render times include graph construction and returned byte copy. No decoding, RAW, ICC, export or controlled power/load."}, indent=2))


if __name__ == "__main__":
    run()
