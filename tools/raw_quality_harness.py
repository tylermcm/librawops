"""Independent procedural RAW reconstruction diagnostics, standard library only.

Ground truth is analytical, never derived from a demosaic implementation. This
diagnostic covers all specified synthetic fixture families in camera-linear RGB. It is not
a file decoder, perceptual/Adobe comparison, or production quality claim.
"""

from __future__ import annotations

import argparse
import array
from dataclasses import dataclass
import hashlib
import gzip
import importlib
import json
import math
from pathlib import Path
import platform
import struct
import sys


REPORT_VERSION = 3
GENERATOR_VERSION = 3
METRIC_VERSION = 1
DOMAIN = "camera-linear-rgb"
CHANNELS = ("R", "G", "B")
PATTERNS = ("RGGB", "BGGR", "GRBG", "GBRG")
CFA = ((0, 1, 1, 2), (2, 1, 1, 0), (1, 0, 2, 1), (1, 2, 0, 1))
TILE_SIZES = (1, 2, 7, 256)


def integer(value, name, minimum=0):
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"invalid {name}")
    return value


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def little_bytes(typecode, values):
    packed = array.array(typecode, values)
    if packed.itemsize != {"f": 4, "H": 2}[typecode]:
        raise ValueError("unsupported native sample size")
    if sys.byteorder != "little":
        packed.byteswap()
    return packed.tobytes()


def digest(data):
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class RgbImage:
    width: int
    height: int
    pixels: tuple
    origin: tuple = (0, 0)
    domain: str = DOMAIN

    @property
    def bounds(self):
        return (*self.origin, self.width, self.height)

    def validate(self):
        integer(self.width, "width")
        integer(self.height, "height")
        if len(self.origin) != 2:
            raise ValueError("invalid image origin")
        for v in self.origin:
            integer(v, "origin")
        if len(self.pixels) != self.width * self.height * 3:
            raise ValueError("RGB shape mismatch")
        if not all(math.isfinite(v) for v in self.pixels):
            raise ValueError("nonfinite RGB sample")


class Errors:
    """Fixed-order float64 accumulation; no float32 reducer arithmetic."""

    def __init__(self):
        self.count = 0
        self.maximum = self.absolute = self.squared = 0.0

    def add(self, error):
        if not math.isfinite(error):
            raise ValueError("nonfinite error")
        self.count += 1
        self.maximum = max(self.maximum, abs(error))
        self.absolute += abs(error)
        self.squared += error * error
        if not math.isfinite(self.absolute) or not math.isfinite(self.squared):
            raise ValueError("metric accumulation overflow")

    def result(self):
        return {"sample_count": self.count,
                "max_abs": self.maximum if self.count else None,
                "mae": self.absolute / self.count if self.count else None,
                "rmse": math.sqrt(self.squared / self.count) if self.count else None}


def scope_indices(image, scope):
    if len(scope) != 4:
        raise ValueError("invalid measurement scope")
    x, y, width, height = (integer(v, "scope") for v in scope)
    ox, oy = image.origin
    if x < ox or y < oy or x + width > ox + image.width or y + height > oy + image.height:
        raise ValueError("measurement scope outside image")
    for yy in range(y - oy, y - oy + height):
        for xx in range(x - ox, x - ox + width):
            yield (yy * image.width + xx) * 3


def measure(rendered, truth, scope=None, *, neutral=False, selection=None):
    """RGB metrics over a rectangle intersected with an optional boolean mask.

    Empty scopes retain zero counts and null metrics. All input samples, even
    outside a selected scope, must be finite and coordinate/domain compatible.
    """
    truth.validate()
    rendered.validate()
    if rendered.bounds != truth.bounds:
        raise ValueError("coordinate/shape mismatch")
    if rendered.domain != truth.domain or truth.domain != DOMAIN:
        raise ValueError("camera-linear domain mismatch")
    if selection is not None and (len(selection) != truth.width * truth.height or
                                  any(type(v) is not bool for v in selection)):
        raise ValueError("invalid measurement selection")
    if neutral and any(truth.pixels[i] != truth.pixels[i + 1] or
                       truth.pixels[i] != truth.pixels[i + 2]
                       for i in range(0, len(truth.pixels), 3)):
        raise ValueError("chroma metric requires neutral ground truth")
    channels = [Errors() for _ in CHANNELS]
    combined, chroma = Errors(), Errors()
    count = 0
    for i in scope_indices(truth, truth.bounds if scope is None else scope):
        if selection is not None and not selection[i // 3]:
            continue
        count += 1
        errors = [float(rendered.pixels[i + c]) - float(truth.pixels[i + c]) for c in range(3)]
        for c, error in enumerate(errors):
            channels[c].add(error)
            combined.add(error)
        if neutral:
            chroma.add(errors[0] - errors[1])
            chroma.add(errors[2] - errors[1])
    chroma_result = chroma.result()
    return {"pixel_count": count,
            "channels": {c: e.result() for c, e in zip(CHANNELS, channels)},
            "rgb": combined.result(),
            "residual_chroma": {k: chroma_result[k] for k in ("sample_count", "max_abs", "rmse")}
            if neutral else None}


@dataclass(frozen=True)
class Probe:
    name: str
    family: str
    neutral: bool
    constant: tuple
    dx: tuple = (0.0, 0.0, 0.0)
    dy: tuple = (0.0, 0.0, 0.0)


PROBES = (
    Probe("neutral_negative", "flat", True, (-0.1,) * 3),
    Probe("neutral_zero", "flat", True, (0.0,) * 3),
    Probe("neutral_mid", "flat", True, (0.37123,) * 3),
    Probe("neutral_headroom", "flat", True, (1.25,) * 3),
    Probe("chromatic_flat", "flat", False, (-0.07321, 0.41723, 1.18321)),
    Probe("neutral_affine", "affine", True, (-0.08321,) * 3, (0.01123,) * 3, (0.00917,) * 3),
    Probe("chromatic_affine", "affine", False, (-0.07123, 0.93217, 0.27123),
          (0.01231, -0.00913, 0.00719), (0.00911, 0.00517, -0.00613)),
)


@dataclass(frozen=True)
class EdgeProbe:
    name: str
    neutral: bool
    low: tuple
    high: tuple
    slope: float = 0.25
    offset: float = 26.375
    transition_width: float = 4.0

    @property
    def family(self):
        return "slanted_edge"

    def validate(self):
        if len(self.low) != 3 or len(self.high) != 3:
            raise ValueError("invalid edge RGB endpoints")
        if any(isinstance(v, bool) or not math.isfinite(v)
               for v in (*self.low, *self.high, self.slope, self.offset, self.transition_width)):
            raise ValueError("nonfinite/invalid edge parameters")
        if self.transition_width not in (0.0, 4.0):
            raise ValueError("edge transition must be a hard step or four native pixels")


# Two non-axis-aligned lines pass through active-local (32.375, 24): a
# near-vertical edge and a near-horizontal edge with the opposite slope.
EDGE_PROBES = tuple(
    EdgeProbe(f"{kind}_{transition}_{direction}", kind == "neutral", low, high,
              slope, offset, width)
    for direction, slope, offset in (("vertical", 0.25, 26.375), ("horizontal", -4.0, 128.375))
    for kind, low, high in (("neutral", (-0.07321,) * 3, (1.18321,) * 3),
                           ("chromatic", (-0.07321, 0.41723, 1.18321), (1.18321, 0.93217, -0.07123)))
    for transition, width in (("linear_edge", 4.0), ("hard_edge", 0.0))
)
@dataclass(frozen=True)
class ImpulseProbe:
    name: str
    neutral: bool
    background: tuple
    peak: tuple
    position: tuple = (32, 24)

    @property
    def family(self):
        return "impulse"

    def validate(self):
        validate_rgb_parameters(self.background, self.peak)
        if len(self.position) != 2:
            raise ValueError("invalid impulse position")
        x, y = (integer(v, "impulse position", 2) for v in self.position)
        if x >= 63 or y >= 47:
            raise ValueError("impulse neighborhood must fit inside the active image")


@dataclass(frozen=True)
class SineProbe:
    name: str
    neutral: bool
    frequency: tuple
    mean: tuple
    amplitude: tuple
    phase_cycles: tuple
    family: str = "smooth_detail"

    def validate(self):
        validate_rgb_parameters(self.mean, self.amplitude, self.phase_cycles)
        if self.family not in ("smooth_detail", "aliasing") or len(self.frequency) != 2:
            raise ValueError("invalid sinusoidal probe")
        if any(isinstance(v, bool) or not math.isfinite(v) or abs(v) > 0.5 for v in self.frequency):
            raise ValueError("invalid sinusoidal frequency")
        if not any(self.frequency) or any(v < 0 for v in self.amplitude):
            raise ValueError("sinusoidal frequency/amplitude must be positive")


@dataclass(frozen=True)
class AlternatingProbe:
    name: str
    neutral: bool
    low: tuple
    high: tuple
    axis: str

    @property
    def family(self):
        return "aliasing"

    def validate(self):
        validate_rgb_parameters(self.low, self.high)
        if self.axis not in ("x", "y", "checkerboard"):
            raise ValueError("invalid alternating axis")


def validate_rgb_parameters(*triples):
    if any(len(v) != 3 for v in triples):
        raise ValueError("invalid RGB parameters")
    if any(isinstance(v, bool) or not math.isfinite(v) for triple in triples for v in triple):
        raise ValueError("nonfinite/invalid RGB parameters")


IMPULSE_PROBES = tuple(
    ImpulseProbe(f"{kind}_impulse", kind == "neutral", (0.125,) * 3,
                 tuple(1.125 if kind == "neutral" or c == channel else 0.125 for c in range(3)))
    for kind, channel in (("neutral", None), ("red", 0), ("green", 1), ("blue", 2))
)


def sine_probes(frequencies, family):
    return tuple(
        SineProbe(f"{kind}_sine_{label}_{axis}", kind == "neutral",
                  (frequency, 0.0) if axis == "x" else (0.0, frequency),
                  (0.55,) * 3 if kind == "neutral" else (0.5, 0.45, 0.6),
                  (0.6,) * 3 if kind == "neutral" else (0.55, 0.5, 0.55),
                  (0.0,) * 3 if kind == "neutral" else (0.0, 0.25, 0.5), family)
        for label, frequency in frequencies for axis in ("x", "y") for kind in ("neutral", "chromatic")
    )


DETAIL_PROBES = sine_probes((("1_32", 1 / 32), ("1_8", 1 / 8)), "smooth_detail")
ALIASING_PROBES = tuple(
    AlternatingProbe(f"{kind}_alternating_{axis}", kind == "neutral",
                     (-0.07321,) * 3 if kind == "neutral" else (-0.07321, 0.41723, 1.18321),
                     (1.18321,) * 3 if kind == "neutral" else (1.18321, 0.93217, -0.07123), axis)
    for axis in ("x", "y", "checkerboard") for kind in ("neutral", "chromatic")
) + sine_probes((("7_16", 7 / 16),), "aliasing")
ALL_PROBES = PROBES + EDGE_PROBES + IMPULSE_PROBES + DETAIL_PROBES + ALIASING_PROBES


def edge_distance(probe, x, y):
    """Signed perpendicular distance to x - slope*y - offset = 0."""
    distance = (x - probe.slope * y - probe.offset) / math.hypot(1.0, probe.slope)
    if not math.isfinite(distance):
        raise ValueError("nonfinite edge distance")
    return distance


def probe_rgb(probe, x, y):
    if isinstance(probe, EdgeProbe):
        distance = edge_distance(probe, x, y)
        blend = (float(distance >= 0.0) if probe.transition_width == 0.0 else
                 min(1.0, max(0.0, 0.5 + distance / probe.transition_width)))
        return tuple(f32(probe.low[c] + (probe.high[c] - probe.low[c]) * blend) for c in range(3))
    if isinstance(probe, ImpulseProbe):
        return tuple(f32(v) for v in (probe.peak if (x, y) == probe.position else probe.background))
    if isinstance(probe, SineProbe):
        cycles = probe.frequency[0] * x + probe.frequency[1] * y
        return tuple(f32(probe.mean[c] + probe.amplitude[c] * math.sin(math.tau * (cycles + probe.phase_cycles[c])))
                     for c in range(3))
    if isinstance(probe, AlternatingProbe):
        parity = (x if probe.axis == "x" else y if probe.axis == "y" else x + y) % 2
        return tuple(f32(v) for v in (probe.high if parity else probe.low))
    return tuple(f32(probe.constant[c] + probe.dx[c] * x + probe.dy[c] * y) for c in range(3))


def probe_parameters(probe):
    common = {"coordinate_rule": "active-local integer pixel centers"}
    if isinstance(probe, EdgeProbe):
        return {**common, "low": probe.low, "high": probe.high, "slope": probe.slope,
                "offset": probe.offset, "transition_width": probe.transition_width,
                "distance_rule": "(x - slope*y - offset) / hypot(1, slope)",
                "transition_rule": "clamp(0.5 + distance/width, 0, 1); hard step uses high at distance >= 0"}
    if isinstance(probe, ImpulseProbe):
        return {**common, "background": probe.background, "peak": probe.peak,
                "position": probe.position, "rule": "peak at the single specified pixel; background elsewhere"}
    if isinstance(probe, SineProbe):
        return {**common, "frequency_cycles_per_pixel_xy": probe.frequency, "mean": probe.mean,
                "amplitude": probe.amplitude, "phase_cycles": probe.phase_cycles,
                "rule": "mean[c] + amplitude[c]*sin(2*pi*(fx*x + fy*y + phase_cycles[c]))",
                "numeric_policy": "float64 Python math.sin, then float32; bit identity requires the pinned environment"}
    if isinstance(probe, AlternatingProbe):
        return {**common, "low": probe.low, "high": probe.high, "axis": probe.axis,
                "rule": "low at even x/y/x+y parity; high at odd parity"}
    return {**common, "constant": probe.constant, "dx": probe.dx, "dy": probe.dy}


def edge_band_selection(image, probe):
    """Select active-local pixel centers with |signed distance| <= 2."""
    if not isinstance(probe, EdgeProbe):
        raise ValueError("edge band requires an edge probe")
    probe.validate()
    image.validate()
    # Ground truth is active-local, so packed/shifted sensors select the same
    # pixels. The reducer still checks original sensor-coordinate alignment.
    return tuple(abs(edge_distance(probe, x, y)) <= 2.0
                 for y in range(image.height) for x in range(image.width))


@dataclass(frozen=True)
class Fixture:
    probe: Probe | EdgeProbe | ImpulseProbe | SineProbe | AlternatingProbe
    layout: str
    level_policy: str
    seed: int
    truth: RgbImage
    sensor_width: int
    sensor_height: int
    metadata: dict
    bayer_le: bytes

    def native_samples(self):
        samples = array.array("H")
        samples.frombytes(self.bayer_le)
        if sys.byteorder != "little":
            samples.byteswap()
        return samples


def site_channel(metadata, x, y):
    # Phase shifts the 2x2 site at original sensor coordinates. Active-area
    # offsets never reset parity. This defines both CFA and black/white sites.
    site = ((y + metadata["cfa_phase_y"]) % 2) * 2 + (x + metadata["cfa_phase_x"]) % 2
    return site, CFA[metadata["pattern"]][site]


def encode_sample(value, black, white):
    if not math.isfinite(value) or white <= black:
        raise ValueError("invalid sensor encoding")
    code = math.floor(black + value * (white - black) + 0.5)
    if code < 0 or code > 65535:
        raise ValueError("fixture code outside uint16; clipping is prohibited")
    return code


def generate(probe, *, layout="packed", pattern=0, phase=(0, 0), levels="uniform", seed=0):
    integer(seed, "seed")  # Analytic fixtures have no PRNG; seed is still recorded.
    integer(pattern, "pattern")
    if pattern >= 4 or len(phase) != 2 or any(integer(v, "phase") > 1 for v in phase):
        raise ValueError("invalid CFA pattern/phase")
    if layout not in ("packed", "shifted") or levels not in ("uniform", "per-site"):
        raise ValueError("invalid fixture layout/levels")
    if isinstance(probe, (EdgeProbe, ImpulseProbe, SineProbe, AlternatingProbe)):
        probe.validate()
    elif probe.family not in ("flat", "affine") or any(len(v) != 3 for v in (probe.constant, probe.dx, probe.dy)):
        raise ValueError("invalid analytic probe")
    width, height = 65, 49
    ox, oy, sw, sh, stride = (0, 0, 65, 49, 65) if layout == "packed" else (1, 3, 69, 55, 72)
    black = (4000,) * 4 if levels == "uniform" else (4000, 7000, 3000, 8000)
    white = (19000,) * 4 if levels == "uniform" else (19000, 23000, 16000, 27000)
    metadata = {"row_stride_samples": stride, "pattern": pattern,
                "cfa_phase_x": phase[0], "cfa_phase_y": phase[1],
                "active_x": ox, "active_y": oy, "active_width": width, "active_height": height,
                "black_levels": black, "white_levels": white}
    pixels = tuple(v for y in range(height) for x in range(width) for v in probe_rgb(probe, x, y))
    if any(not math.isfinite(v) or v < f32(-0.1) or v > 1.25 for v in pixels):
        raise ValueError("fixture RGB outside specified signed/headroom range")
    if probe.neutral and any(pixels[i] != pixels[i + 1] or pixels[i] != pixels[i + 2]
                             for i in range(0, len(pixels), 3)):
        raise ValueError("neutral probe has chromatic ground truth")
    # Deliberately hostile inactive/padding values; excluded from truth/metrics.
    samples = [65535] * (stride * sh)
    for y in range(height):
        for x in range(width):
            site, channel = site_channel(metadata, ox + x, oy + y)
            samples[(oy + y) * stride + ox + x] = encode_sample(
                pixels[(y * width + x) * 3 + channel], black[site], white[site])
    return Fixture(probe, layout, levels, seed, RgbImage(width, height, pixels, (ox, oy)),
                   sw, sh, metadata, little_bytes("H", samples))


def observed_metrics(fixture, rendered):
    """Separate encoded-sample quantization from matching-channel fidelity."""
    fixture.truth.validate()
    rendered.validate()
    if rendered.bounds != fixture.truth.bounds or rendered.domain != DOMAIN:
        raise ValueError("observed sample coordinate/domain mismatch")
    samples = fixture.native_samples()
    quantization, fidelity = [Errors() for _ in CHANNELS], [Errors() for _ in CHANNELS]
    quant_rgb, fidelity_rgb = Errors(), Errors()
    ox, oy = fixture.truth.origin
    meta = fixture.metadata
    for y in range(fixture.truth.height):
        for x in range(fixture.truth.width):
            site, c = site_channel(meta, ox + x, oy + y)
            normalized = (samples[(oy + y) * meta["row_stride_samples"] + ox + x] - meta["black_levels"][site]) / (
                meta["white_levels"][site] - meta["black_levels"][site])
            i = (y * fixture.truth.width + x) * 3 + c
            q = normalized - fixture.truth.pixels[i]
            e = rendered.pixels[i] - normalized
            quantization[c].add(q)
            fidelity[c].add(e)
            quant_rgb.add(q)
            fidelity_rgb.add(e)
    def result(channels, combined):
        return {"channels": {c: e.result() for c, e in zip(CHANNELS, channels)}, "observed": combined.result()}
    return {"quantization": result(quantization, quant_rgb), "observed_fidelity": result(fidelity, fidelity_rgb)}


def decode_render(result, bounds):
    x, y, width, height = bounds
    if result[:2] != (width, height) or len(result[2]) != width * height * 12:
        raise ValueError("render shape mismatch")
    pixels = array.array("f")
    pixels.frombytes(result[2])  # Python binding returns native-endian float32.
    image = RgbImage(width, height, tuple(pixels), (x, y))
    image.validate()
    return image


def crop_bytes(result, full_bounds, roi):
    x, y, width, height = roi
    ox, oy, full_width, _ = full_bounds
    return b"".join(result[2][((yy - oy) * full_width + x - ox) * 12:
                             ((yy - oy) * full_width + x - ox + width) * 12]
                    for yy in range(y, y + height))


def render_case(raw, fixture):
    session = raw.RawSession(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height,
                             fixture.metadata, cache_bytes=0)
    try:
        manifest = json.loads(session.export_manifest())
        source_info = session.source_info()
        manifest["operations"] = []
        manifest["output"] = source_info["id"]
        text = json.dumps(manifest, sort_keys=True, separators=(",", ":"), allow_nan=False)
        bounds = fixture.truth.bounds
        x, y, width, height = bounds
        request = {"x": x, "y": y, "roi_width": width, "roi_height": height,
                   "mip": 0, "quality": "final", "tile_size": 256}
        result = session.render_manifest(text, request)
        rendered = decode_render(result, bounds)
        interior = (x + 1, y + 1, width - 2, height - 2)
        metrics = {"whole": measure(rendered, fixture.truth, neutral=fixture.probe.neutral),
                   "interior": measure(rendered, fixture.truth, interior, neutral=fixture.probe.neutral),
                   **observed_metrics(fixture, rendered)}
        scopes = {"whole": bounds, "interior": interior, "interior_margin": 1}
        if isinstance(fixture.probe, EdgeProbe):
            selection = edge_band_selection(fixture.truth, fixture.probe)
            metrics["edge_band"] = measure(rendered, fixture.truth, neutral=fixture.probe.neutral,
                                            selection=selection)
            scopes["edge_band"] = {"kind": "analytic signed-distance selection", "half_width": 2.0,
                                   "inclusive": True, "coordinate_rule": "active-local integer pixel centers",
                                   "pixel_count": sum(selection), "selection_sha256": digest(bytes(selection))}
        if isinstance(fixture.probe, ImpulseProbe):
            px, py = fixture.probe.position
            center = (x + px, y + py, 1, 1)
            neighborhood = (x + px - 2, y + py - 2, 5, 5)
            surround = tuple((xx, yy) != (px, py) for yy in range(height) for xx in range(width))
            metrics["impulse_center"] = measure(rendered, fixture.truth, center, neutral=fixture.probe.neutral)
            metrics["impulse_neighborhood"] = measure(rendered, fixture.truth, neighborhood, neutral=fixture.probe.neutral)
            metrics["impulse_surround"] = measure(rendered, fixture.truth, neighborhood,
                                                  neutral=fixture.probe.neutral, selection=surround)
            scopes.update({"impulse_center": center, "impulse_neighborhood": neighborhood,
                           "impulse_surround": {"bounds": neighborhood, "excluded_pixel": center[:2],
                                                "pixel_count": 24, "selection_sha256": digest(bytes(surround))}})
        tiles = {str(size): session.render_manifest(text, {**request, "tile_size": size}) == result
                 for size in TILE_SIZES}
        # Top/left, odd interior, bottom/right, and the complete active area.
        rois = ((x, y, 1, height), (x + 3, y + 5, 17, 11),
                (x + width - 1, y + height - 1, 1, 1), bounds)
        roi_checks = []
        for roi in rois:
            rr = {**request, "x": roi[0], "y": roi[1], "roi_width": roi[2], "roi_height": roi[3], "tile_size": 7}
            actual = session.render_manifest(text, rr)
            decode_render(actual, roi)
            roi_checks.append({"bounds": roi, "exact_crop": actual[2] == crop_bytes(result, bounds, roi)})
        bound = max(0.5 / (w - b) for b, w in zip(fixture.metadata["black_levels"], fixture.metadata["white_levels"]))
        analytical_scope = {"flat": "whole", "affine": "interior"}.get(fixture.probe.family)
        gates = {"analytical_reconstruction": (metrics[analytical_scope]["rgb"]["max_abs"] <= bound + 1e-6
                                                if analytical_scope is not None else None),
                 "quantization_bound": metrics["quantization"]["observed"]["max_abs"] <= bound + 1e-12,
                 "observed_sample_fidelity": metrics["observed_fidelity"]["observed"]["max_abs"] <= 1e-6,
                 "tile_exact": all(tiles.values()), "roi_exact": all(r["exact_crop"] for r in roi_checks)}
        return {"fixture": {"name": fixture.probe.name, "family": fixture.probe.family,
                            "neutral": fixture.probe.neutral, "seed": fixture.seed,
                            "parameters": probe_parameters(fixture.probe),
                            "layout": fixture.layout, "level_policy": fixture.level_policy,
                            "ground_truth": {"encoding": "little-endian interleaved float32 RGB",
                                             "bounds": bounds, "domain": DOMAIN,
                                             "sha256": digest(little_bytes("f", fixture.truth.pixels))},
                            "bayer": {"encoding": "little-endian uint16; complete padded sensor buffer",
                                      "sha256": digest(fixture.bayer_le)},
                            "sensor_width": fixture.sensor_width, "sensor_height": fixture.sensor_height,
                            "metadata": fixture.metadata, "pattern_name": PATTERNS[fixture.metadata["pattern"]]},
                "source_info": source_info, "saved_manifest": manifest, "render_request": request,
                "rendered_float32_le_sha256": digest(little_bytes("f", rendered.pixels)),
                "measurement_scopes": scopes,
                "metrics": metrics, "tile_exact": tiles, "roi_checks": roi_checks,
                "acceptance": {"analytical_scope": analytical_scope,
                               "quality_role": "analytical correctness" if analytical_scope is not None else "baseline characterization",
                               "quantization_max_bound": bound,
                               "reconstruction_max_limit": bound + 1e-6 if analytical_scope is not None else None,
                               "observed_fidelity_max_limit": 1e-6, "gates": gates,
                               "passed": all(v for v in gates.values() if v is not None)}}
    finally:
        session.close()


def build_record(module_dir, raw):
    cache = Path(module_dir) / "CMakeCache.txt"
    keys = ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS_RELEASE",
            "CMAKE_GENERATOR", "RAWENGINE_BUILD_PYTHON", "RAWENGINE_USE_OPENMP", "RAWENGINE_WITH_LCMS")
    configuration = {}
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            if "=" in line and ":" in line.split("=", 1)[0]:
                key = line.split(":", 1)[0]
                if key in keys:
                    configuration[key] = line.split("=", 1)[1]
    artifacts = {Path(raw.__file__).name: digest(Path(raw.__file__).read_bytes())}
    dll = Path(raw.__file__).parent / "RawEngine.dll"
    if dll.is_file():
        artifacts[dll.name] = digest(dll.read_bytes())
    return {"configuration": configuration or {"status": "CMake cache unavailable; describe external build"},
            "artifact_sha256": artifacts}


def report(raw, *, seed=0, sweep=True, build=None):
    integer(seed, "seed")
    cases = []
    for probe in ALL_PROBES:
        for layout in ("packed", "shifted"):
            for pattern in range(4) if sweep else (0,):
                for phase in ((0, 0), (1, 0), (0, 1), (1, 1)) if sweep else ((0, 0),):
                    for levels in ("uniform", "per-site"):
                        cases.append(render_case(raw, generate(probe, layout=layout, pattern=pattern,
                                                               phase=phase, levels=levels, seed=seed)))
    return {"report_schema_version": REPORT_VERSION, "generator_version": GENERATOR_VERSION,
            "metric_formula_version": METRIC_VERSION,
            "seed": seed, "randomness": "none; analytic fixtures", "provenance": "original procedural test data",
            "scope": "complete specification-v1 synthetic corpus; representative cameras and preview consistency are separate",
            "algorithm": {"name": "existing native bilinear baseline", "version": "bilinear-reference-v1",
                          "demosaic": cases[0]["source_info"].get("demosaic", {"algorithm": "rawengine.bilinear", "processing_version": 1}),
                          "measurement_domain": DOMAIN, "stage": "RAW source only; before WB/exposure/calibration/output",
                          "core_source_sha256": digest((Path(__file__).resolve().parents[1] / "RawEngine.cpp").read_bytes())},
            "runtime": {"python": sys.version, "implementation": platform.python_implementation(),
                        "platform": platform.platform(), "machine": platform.machine(), "byte_order": sys.byteorder},
            "build": build or {"status": "unspecified"},
            "summary": {"case_count": len(cases), "passed_cases": sum(c["acceptance"]["passed"] for c in cases),
                        "probe_count": len(ALL_PROBES), "metadata_sweep": "full" if sweep else "quick",
                        "family_cases": {family: sum(c["fixture"]["family"] == family for c in cases)
                                         for family in ("flat", "affine", "slanted_edge", "impulse", "smooth_detail", "aliasing")},
                        "analytical_cases": sum(c["acceptance"]["analytical_scope"] is not None for c in cases),
                        "characterization_cases": sum(c["acceptance"]["analytical_scope"] is None for c in cases),
                        "passed": all(c["acceptance"]["passed"] for c in cases)}, "cases": cases}


def report_json(record):
    return json.dumps(record, indent=2, sort_keys=True, allow_nan=False) + "\n"


def validate_baseline(record, *, require_passed=True):
    """A preserved baseline must contain the complete, successful seed-0 sweep."""
    if (record["report_schema_version"] != REPORT_VERSION or record["generator_version"] != GENERATOR_VERSION or
            record["metric_formula_version"] != METRIC_VERSION or record["seed"] != 0):
        raise ValueError("baseline versions/seed differ from the pinned corpus")
    expected = {(probe.name, layout, pattern, px, py, levels)
                for probe in ALL_PROBES for layout in ("packed", "shifted") for pattern in range(4)
                for px in range(2) for py in range(2) for levels in ("uniform", "per-site")}
    definitions = {probe.name: (probe.family, probe.neutral, report_json(probe_parameters(probe)))
                   for probe in ALL_PROBES}
    keys = []
    for case in record["cases"]:
        fixture = case["fixture"]
        definition = (fixture["family"], fixture["neutral"], report_json(fixture["parameters"]))
        if definitions.get(fixture["name"]) != definition or fixture["seed"] != 0:
            raise ValueError("baseline fixture definition differs from the pinned corpus")
        metadata = fixture["metadata"]
        keys.append((fixture["name"], fixture["layout"], metadata["pattern"], metadata["cfa_phase_x"],
                     metadata["cfa_phase_y"], fixture["level_policy"]))
        gates = case["acceptance"]["gates"]
        required = {"quantization_bound", "observed_sample_fidelity", "tile_exact", "roi_exact"}
        analytical = fixture["family"] in ("flat", "affine")
        if require_passed and (set(gates) != required | {"analytical_reconstruction"} or
                any(gates[k] is not True for k in required) or
                gates["analytical_reconstruction"] is not (True if analytical else None) or
                case["acceptance"]["passed"] is not True):
            raise ValueError("cannot preserve a failed correctness case")
    summary = record["summary"]
    if (len(keys) != len(expected) or set(keys) != expected or summary["case_count"] != len(expected) or
            summary["passed_cases"] != sum(c["acceptance"]["passed"] is True for c in record["cases"]) or
            summary["passed"] is not all(c["acceptance"]["passed"] is True for c in record["cases"]) or
            (require_passed and summary["passed"] is not True) or
            summary["metadata_sweep"] != "full" or summary["probe_count"] != len(ALL_PROBES)):
        raise ValueError("baseline requires the complete metadata sweep without duplicate/missing cases")
    actual_families = {family: sum(c["fixture"]["family"] == family for c in record["cases"])
                       for family in ("flat", "affine", "slanted_edge", "impulse", "smooth_detail", "aliasing")}
    analytical_count = actual_families["flat"] + actual_families["affine"]
    if (summary["family_cases"] != actual_families or summary["analytical_cases"] != analytical_count or
            summary["characterization_cases"] != len(expected) - analytical_count):
        raise ValueError("baseline summary counts differ from its cases")
    # Also fail closed on nonfinite values anywhere in the report.
    report_json(record)


def preserve_baseline(path, record):
    """Write deterministic gzip evidence once; differing existing files reject."""
    validate_baseline(record)
    report_bytes = report_json(record).encode("utf-8")
    payload = gzip.compress(report_bytes, compresslevel=9, mtime=0)
    path = Path(path)
    index = baseline_index_path(path)
    index_bytes = report_json(baseline_index(record, path.name, payload, report_bytes)).encode("utf-8")
    for target, content in ((path, payload), (index, index_bytes)):
        if target.exists() and target.read_bytes() != content:
            raise FileExistsError(f"existing baseline differs; use a versioned new path: {target}")
    path.parent.mkdir(parents=True, exist_ok=True)
    for target, content in ((path, payload), (index, index_bytes)):
        if not target.exists():
            with target.open("xb") as output:
                output.write(content)
    return digest(payload)


def baseline_index_path(path):
    path = Path(path)
    if not path.name.endswith(".json.gz"):
        raise ValueError("baseline path must end in .json.gz")
    return path.with_name(path.name[:-8] + ".index.json")


def baseline_index(record, filename, payload, report_bytes):
    """Human-readable maxima, with hashes linking to the full case evidence."""
    probes = []
    for probe in ALL_PROBES:
        cases = [c for c in record["cases"] if c["fixture"]["name"] == probe.name]
        scores = {}
        for scope in ("whole", "interior", "edge_band", "impulse_center", "impulse_neighborhood", "impulse_surround"):
            if scope not in cases[0]["metrics"]:
                continue
            metrics = [c["metrics"][scope] for c in cases]
            scores[scope] = {"max_case_rgb_" + name: max(m["rgb"][name] for m in metrics)
                             for name in ("max_abs", "mae", "rmse")}
            scores[scope]["max_case_residual_chroma_rmse"] = (
                max(m["residual_chroma"]["rmse"] for m in metrics) if probe.neutral else None)
        probes.append({"name": probe.name, "family": probe.family, "neutral": probe.neutral,
                       "case_count": len(cases), "parameters": probe_parameters(probe), "scope_maxima": scores})
    return {"index_version": 1, "baseline_file": filename, "compressed_sha256": digest(payload),
            "report_json_sha256": digest(report_bytes), "report_schema_version": record["report_schema_version"],
            "generator_version": record["generator_version"], "metric_formula_version": record["metric_formula_version"],
            "provenance": record["provenance"], "algorithm": record["algorithm"], "runtime": record["runtime"],
            "build": record["build"], "summary": record["summary"],
            "quality_policy": "characterization has no reconstruction acceptance threshold; review before a replacement algorithm",
            "probe_maxima": probes}


def read_baseline(path):
    path = Path(path)
    payload = path.read_bytes()
    index = json.loads(baseline_index_path(path).read_text(encoding="utf-8"))
    if index["baseline_file"] != path.name or index["compressed_sha256"] != digest(payload):
        raise ValueError("baseline compressed identity differs from its index")
    report_bytes = gzip.decompress(payload)
    if index["report_json_sha256"] != digest(report_bytes):
        raise ValueError("baseline JSON identity differs from its index")
    record = json.loads(report_bytes)
    validate_baseline(record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module_dir", type=Path, help="directory containing rawengine_native")
    parser.add_argument("output", type=Path, help="JSON report destination")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--quick", action="store_true", help="RGGB/phase 0 only; both layouts and level policies")
    parser.add_argument("--build-description", default="", help="extra compiler/runtime/build context")
    parser.add_argument("--preserve-baseline", type=Path, help="preserve the full seed-0 report as immutable .json.gz evidence")
    args = parser.parse_args()
    if args.preserve_baseline and (args.quick or args.seed != 0):
        parser.error("preserving a baseline requires the full seed-0 sweep")
    sys.path.insert(0, str(args.module_dir.resolve()))
    raw = importlib.import_module("rawengine_native")
    build = build_record(args.module_dir, raw)
    build["description"] = args.build_description
    record = report(raw, seed=args.seed, sweep=not args.quick, build=build)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write(report_json(record))
    print(json.dumps(record["summary"], sort_keys=True))
    if args.preserve_baseline:
        print(json.dumps({"baseline_sha256": preserve_baseline(args.preserve_baseline, record)}, sort_keys=True))
    return 0 if record["summary"]["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
