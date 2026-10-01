"""Trace unchanged Menon v1 on five predeclared frozen synthetic fixtures.

Diagnostic radius-eight summaries and worst-excess pixels are explanatory,
not acceptance scopes or an evaluated replacement. Pillow is optional and
used only to publish an illustrative figure; numerical work uses stdlib.
"""
from __future__ import annotations

import argparse
import gzip
import json
import math
from pathlib import Path
import sys

import raw_ha_reference as ha
import raw_menon_reference as m
import raw_quality_compare as comparison
import raw_quality_harness as h

PROBES = ("chromatic_hard_edge_vertical", "chromatic_linear_edge_vertical",
          "chromatic_sine_1_32_x", "chromatic_sine_1_8_x", "neutral_hard_edge_vertical")
SELECTION = "maximum base squared error minus bilinear squared error among missing channels with margin >= 8; ties: nearest active center, then y/x/channel"
STAGES = ("base", "green_refined", "green_sites_refined", "refined")


def read_study(path):
    path = Path(path)
    packed = path.read_bytes()
    index = json.loads(path.with_suffix("").with_suffix(".index.json").read_text(encoding="utf-8"))
    if index["research_file"] != path.name or index["compressed_sha256"] != h.digest(packed):
        raise ValueError("Menon study/index identity mismatch")
    payload = gzip.decompress(packed)
    if index["report_json_sha256"] != h.digest(payload):
        raise ValueError("Menon study/index identity mismatch")
    study = json.loads(payload)
    validate_study(study)
    return study


def validate_study(study):
    if (study["research_report_schema_version"] != 2 or study["reference_version"] != 1
            or study["paired_case_count"] != 2368 or study["metadata_sweep"] != "full"
            or study["native_replacement_accepted"] is not False
            or study["reference_source_sha256_lf"] != m.source_digest(m)
            or study["policy_sha256"] != h.digest(comparison.POLICY_PATH.read_bytes())
            or study["policy"] != json.loads(comparison.POLICY_PATH.read_text(encoding="utf-8"))):
        raise ValueError("Menon study provenance/contract mismatch")
    for module in (h, ha, m.support, m.corpus, comparison):
        if study["helper_source_sha256_lf"][Path(module.__file__).name] != m.source_digest(module):
            raise ValueError("Menon helper provenance mismatch")
    for name, refine in (("base", False), ("refined", True)):
        variant = study["variants"][name]
        if (variant["case_count"] != 2368 or len(variant["cases"]) != 2368
                or variant["native_replacement_accepted"] is not False
                or variant["research_identity"] != {"algorithm": "librawops.menon.scalar-research", "version": 1,
                    "refinement": refine, "float32_stages": True, "bilinear_border": 8 if refine else 6}):
            raise ValueError("Menon variant identity mismatch")


def truth_at(fixture, p, channel):
    x, y = p[0] - fixture.truth.origin[0], p[1] - fixture.truth.origin[1]
    if not (0 <= x < fixture.truth.width and 0 <= y < fixture.truth.height):
        raise ValueError("trace truth coordinate outside active bounds")
    return fixture.truth.pixels[(y * fixture.truth.width + x) * 3 + channel]


def stage_value(ref, stage, p, c):
    if stage == "observed":
        if ref.colors[p] != c:
            raise ValueError("trace requests an unobserved channel")
        return ref.observed[p]
    if stage == "green":
        if c != 1:
            raise ValueError("green trace requires channel G")
        return ref.green(p)
    if stage == "green_refined":
        return ref.refined_green(p) if c == 1 else ref.base(p, c)
    if stage == "green_sites_refined":
        return ref.after_green_locations(p, c)
    if stage == "base":
        return ref.base(p, c)
    if stage == "refined":
        return ref.refined(p, c)
    raise ValueError("unknown trace stage")


def linear_trace(fixture, ref, p, c, terms, output, unrounded):
    """Close the error into ideal stencil, input error and arithmetic/storage.

    Ideal stencil evaluates the same coefficients on stored float32 truth.
    Input error includes quantization and errors in named earlier stages;
    it is not a fitted quantization allowance or an acceptance threshold.
    """
    rows = []
    for q, channel, stage, weight in terms:
        value, truth = stage_value(ref, stage, q, channel), truth_at(fixture, q, channel)
        rows.append({"sensor": list(q), "active_local": [q[i] - fixture.truth.origin[i] for i in (0, 1)],
                     "channel": h.CHANNELS[channel], "source_stage": stage, "weight": weight,
                     "value": value, "truth": truth, "weighted_input_error": weight * (value - truth)})
    target = truth_at(fixture, p, c)
    ideal = math.fsum(row["weight"] * row["truth"] for row in rows)
    components = {"ideal_stencil_model_error": ideal - target,
                  "propagated_input_error": math.fsum(row["weighted_input_error"] for row in rows),
                  "binary64_expression_roundoff": unrounded - math.fsum(row["weight"] * row["value"] for row in rows),
                  "float32_store_roundoff": output - unrounded}
    error = output - target
    closure = error - math.fsum(components.values())
    if abs(closure) > 2e-14:
        raise ValueError("stage decomposition failed numerical closure")
    return {"sensor": list(p), "channel": h.CHANNELS[c], "truth": target, "value": output,
            "error": error, "unrounded_expression": unrounded, "ideal_stencil_value": ideal,
            "error_components": components, "closure_residual": closure, "terms": rows}


def directional_trace(fixture, ref, p, axis):
    own, e, s = ref.colors[p], m.AXES[axis], ref.observed
    a, b = m.support.shifted(p, e, -1), m.support.shifted(p, e, 1)
    far_a, far_b = m.support.shifted(p, e, -2), m.support.shifted(p, e, 2)
    terms = [(a, 1, "observed", .5), (b, 1, "observed", .5),
             (p, own, "observed", .5), (far_a, own, "observed", -.25), (far_b, own, "observed", -.25)]
    unrounded = .5 * (s[a] + s[b]) + .25 * ((2 * s[p] - s[far_a]) - s[far_b])
    return linear_trace(fixture, ref, p, 1, terms, ref.directional_green(p, axis), unrounded)


def stage_trace(fixture, ref, p, c, stage):
    own = ref.colors[p]
    if c == own:
        return linear_trace(fixture, ref, p, c, [(p, c, "observed", 1)], ref.observed[p], ref.observed[p])
    if stage == "base":
        if c == 1:
            return directional_trace(fixture, ref, p, ref.direction(p))
        if own == 1:
            axis = 0 if ref.colors[p[0] + 1, p[1]] == c else 1
            center, neighbor = "green", "observed"
            other, other_stage = 1, "green"
        else:
            axis, center, neighbor, other, other_stage = ref.direction(p), "observed", "base", own, "base"
        e = m.AXES[axis]
        a, b = m.support.shifted(p, e, -1), m.support.shifted(p, e, 1)
        terms = [(p, own, center, 1), (a, c, neighbor, .5), (a, other, other_stage, -.5),
                 (b, c, neighbor, .5), (b, other, other_stage, -.5)]
        unrounded = stage_value(ref, center, p, own) + .5 * ((stage_value(ref, neighbor, a, c) - stage_value(ref, other_stage, a, other)) +
                                                          (stage_value(ref, neighbor, b, c) - stage_value(ref, other_stage, b, other)))
    elif stage == "green_refined":
        if c != 1:
            return stage_trace(fixture, ref, p, c, "base")
        e = m.AXES[ref.direction(p)]
        points = (m.support.shifted(p, e, -1), p, m.support.shifted(p, e, 1))
        terms = [(p, own, "observed", 1)]
        for q in points:
            terms.extend(((q, own, "base", -1 / 3), (q, 1, "green", 1 / 3)))
        delta = [ref.base(q, own) - ref.green(q) for q in points]
        unrounded = ref.observed[p] - ((delta[0] + delta[1]) + delta[2]) / 3
    elif stage == "green_sites_refined":
        if c == 1:
            return stage_trace(fixture, ref, p, c, "green_refined")
        if own != 1:
            return stage_trace(fixture, ref, p, c, "base")
        e = m.AXES[0 if ref.colors[p[0] + 1, p[1]] == c else 1]
        a, b = m.support.shifted(p, e, -1), m.support.shifted(p, e, 1)
        terms = [(p, 1, "observed", 1), (a, c, "observed", .5), (a, 1, "green_refined", -.5),
                 (b, c, "observed", .5), (b, 1, "green_refined", -.5)]
        unrounded = ref.observed[p] + .5 * ((ref.observed[a] - ref.refined_green(a)) + (ref.observed[b] - ref.refined_green(b)))
    elif stage == "refined":
        if c == 1 or own == 1:
            return stage_trace(fixture, ref, p, c, "green_sites_refined")
        e = m.AXES[ref.direction(p)]
        points = (m.support.shifted(p, e, -1), p, m.support.shifted(p, e, 1))
        terms = [(p, own, "observed", 1)]
        for q in points:
            terms.extend(((q, c, "green_sites_refined", 1 / 3), (q, own, "green_sites_refined", -1 / 3)))
        delta = [ref.after_green_locations(q, c) - ref.after_green_locations(q, own) for q in points]
        unrounded = ref.observed[p] + ((delta[0] + delta[1]) + delta[2]) / 3
    else:
        raise ValueError("unknown completed stage")
    return linear_trace(fixture, ref, p, c, terms, stage_value(ref, stage, p, c), unrounded)


def green_choices(fixture, ref, p):
    if ref.colors[p] == 1:
        raise ValueError("direction trace requires red/blue site")
    directions = [directional_trace(fixture, ref, p, axis) for axis in (0, 1)]
    return {"sensor": list(p), "observed_channel": h.CHANNELS[ref.colors[p]],
            "classifiers_H_V": list(ref.classifiers(p)), "selected": "HV"[ref.direction(p)],
            "predictors_H_V": directions,
            "note": "alternate green predictor only; later stages remain unchanged; no counterfactual candidate evaluated"}


def select_pixel(fixture, ref, images):
    ox, oy, w, height = ref.bounds
    candidates = []
    for y in range(oy + 8, oy + height - 8):
        for x in range(ox + 8, ox + w - 8):
            for c in range(3):
                if ref.colors[x, y] == c:
                    continue
                i = ((y - oy) * w + x - ox) * 3 + c
                truth = fixture.truth.pixels[i]
                excess = (images["base"].pixels[i] - truth) ** 2 - (images["bilinear"].pixels[i] - truth) ** 2
                distance = (2 * (x - ox) - (w - 1)) ** 2 + (2 * (y - oy) - (height - 1)) ** 2
                candidates.append((-excess, distance, y, x, c))
    _, _, y, x, c = min(candidates)
    return (x, y), c


def diagnose_case(fixture, old, expected):
    info = old["fixture"]
    if (comparison.canonical(fixture.metadata) != comparison.canonical(info["metadata"])
            or h.digest(fixture.bayer_le) != info["bayer"]["sha256"]
            or h.digest(h.little_bytes("f", fixture.truth.pixels)) != info["ground_truth"]["sha256"]):
        raise ValueError("diagnostic fixture differs from preserved evidence")
    ref = m.BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
    ox, oy, w, height = ref.bounds
    bilinear = h.RgbImage(w, height, tuple(ref.bilinear((x, y), c) for y in range(oy, oy + height)
                          for x in range(ox, ox + w) for c in range(3)), (ox, oy))
    images = {"truth": fixture.truth, "bilinear": bilinear, "base": ref.render(refine=False), "refined": ref.render()}
    hashes = {name: h.digest(h.little_bytes("f", im.pixels)) for name, im in images.items()}
    frozen_metrics = {name: ha.case_metrics(fixture, images[name]) for name in ("bilinear", "base", "refined")}
    for name, evidence in (("bilinear", old), ("base", expected["base"]), ("refined", expected["refined"])):
        if hashes[name] != evidence["rendered_float32_le_sha256"] or frozen_metrics[name] != evidence["metrics"]:
            raise ValueError("diagnostic render/metrics differ from preserved evidence")
    point, channel = select_pixel(fixture, ref, images)
    stage_errors = {stage: [[], [], []] for stage in STAGES}
    green = {"selected_squared_error": [], "best_of_H_V_squared_error": [], "H_squared_error": [], "V_squared_error": []}
    green_count, worse_count = 0, 0
    for y in range(oy + 8, oy + height - 8):
        for x in range(ox + 8, ox + w - 8):
            p = x, y
            for stage in STAGES:
                for c in range(3):
                    stage_errors[stage][c].append((stage_value(ref, stage, p, c) - truth_at(fixture, p, c)) ** 2)
            if ref.colors[p] != 1:
                errors = [(ref.directional_green(p, axis) - truth_at(fixture, p, 1)) ** 2 for axis in (0, 1)]
                chosen = errors[ref.direction(p)]
                for name, value in zip(green, (chosen, min(errors), *errors)):
                    green[name].append(value)
                green_count += 1
                worse_count += chosen > min(errors)
    traces = {stage: stage_trace(fixture, ref, point, channel, stage) for stage in STAGES}
    upstream, green_points = [], set()
    for term in traces["base"]["terms"]:
        q, c = tuple(term["sensor"]), h.CHANNELS.index(term["channel"])
        if term["source_stage"] == "base" and ref.colors[q] != c:
            trace = stage_trace(fixture, ref, q, c, "base")
            upstream.append(trace)
            green_points.update(tuple(row["sensor"]) for row in trace["terms"] if row["source_stage"] == "green")
        elif term["source_stage"] == "green":
            green_points.add(q)
    if ref.colors[point] != 1:
        green_points.add(point)
    neighbor_choices = [green_choices(fixture, ref, q) for q in sorted(green_points, key=lambda p: (p[1], p[0]))
                        if ref.colors[q] != 1]
    px, py = point[0] - ox, point[1] - oy
    crop = (max(0, min(px - 12, w - 25)), max(0, min(py - 10, height - 21)), 25, 21)
    frozen_scope = json.loads(comparison.POLICY_PATH.read_text(encoding="utf-8"))["improvement_scopes"][fixture.probe.family]
    record = {"case": list(comparison.case_key(old)), "fixture": info, "rendered_float32_le_sha256": hashes,
              "frozen_metrics": frozen_metrics, "frozen_improvement_scope": frozen_scope,
              "diagnostic_scope_sensor": [ox + 8, oy + 8, w - 16, height - 16],
              "diagnostic_scope_note": "common complete-support interior for stage attribution only; frozen metrics above retain their original scopes",
              "stage_channel_rmse": {stage: dict(zip(h.CHANNELS, (math.sqrt(math.fsum(errors) / len(errors))
                                             for errors in channels))) for stage, channels in stage_errors.items()},
              "green_direction_summary": {**{name: math.fsum(values) for name, values in green.items()},
                    "red_blue_site_count": green_count, "strictly_worse_than_other_predictor_count": worse_count,
                    "note": "truth-dependent attribution, not an implementable direction rule"},
              "selected_pixel": {"sensor": list(point), "active_local": [px, py], "channel": h.CHANNELS[channel],
                    "observed_channel": h.CHANNELS[ref.colors[point]], "selection": SELECTION,
                    "values": {name: im.pixels[(py * w + px) * 3 + channel] for name, im in images.items()},
                    "stage_traces": traces, "upstream_base_input_traces": upstream,
                    "green_choices": neighbor_choices},
              "crop_active_local": list(crop),
              "profile": {"active_local_y": py, "channel": h.CHANNELS[channel],
                    "values": {name: [im.pixels[(py * w + x) * 3 + channel] for x in range(w)] for name, im in images.items()}}}
    return record, images


def evaluate(baseline, study):
    h.validate_baseline(baseline)
    validate_study(study)
    if study["baseline_report_sha256"] != h.digest(h.report_json(baseline).encode()):
        raise ValueError("diagnostic baseline/study pairing mismatch")
    old_by_key = {comparison.case_key(case): case for case in baseline["cases"]}
    by_variant = {name: {tuple(case["case"]): case for case in variant["cases"]} for name, variant in study["variants"].items()}
    probes, cases, frames = {p.name: p for p in h.ALL_PROBES}, [], []
    for name in PROBES:
        key = name, "packed", 0, 0, 0, "uniform"
        fixture = h.generate(probes[name])
        case, images = diagnose_case(fixture, old_by_key[key], {name: mapping[key] for name, mapping in by_variant.items()})
        cases.append(case)
        frames.append(images)
    record = {"diagnostic_schema_version": 1, "diagnostic_source_sha256_lf": m.source_digest(sys.modules[__name__]),
              "baseline_report_sha256": study["baseline_report_sha256"],
              "scalar_report_sha256": h.digest(h.report_json(study).encode()),
              "reference_source_sha256_lf": study["reference_source_sha256_lf"], "policy_sha256": study["policy_sha256"],
              "measurement_domain": h.DOMAIN, "native_replacement_accepted": False,
              "scope": "five fixed metadata cases; stage attribution and illustrative crops only; no new quality gate, algorithm or camera evidence",
              "display": {"mapping": "clip camera-linear RGB to [0,1], then sRGB OETF per channel; no calibration/WB; nearest-neighbor 8x",
                          "signed_headroom_values_retained_in_json": True, "marker": "selected pixel for its named target channel"},
              "cases": cases}
    return record, frames


def display_byte(value):
    value = min(1.0, max(0.0, value))
    encoded = 12.92 * value if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055
    return round(255 * encoded)


def publish_figure(path, record, frames):
    # Optional research publishing dependency, never needed by numerical tests.
    from PIL import Image, ImageDraw, ImageFont
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 14)
        title_font = ImageFont.truetype("DejaVuSans.ttf", 22)
    except OSError:
        font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 14)
        title_font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 22)
    canvas = Image.new("RGB", (1440, 1320), "#101821")
    draw = ImageDraw.Draw(canvas)
    draw.text((24, 18), "Menon v1: chromatic failure traces and a neutral control", font=title_font, fill="white")
    draw.text((24, 52), "Five fixed RGGB / phase (0,0) / packed / uniform-level synthetic cases. Both Menon variants remain rejected.", font=font, fill="#bbc9d8")
    draw.text((24, 76), "Crops: clipped camera RGB + sRGB transfer, 8x nearest; no calibration/WB. JSON retains signed/headroom values and exact metrics.", font=font, fill="#bbc9d8")
    names = ("truth", "bilinear", "base", "refined")
    colors = {"truth": "#e8ecf1", "bilinear": "#66d6dd", "base": "#ffb45f", "refined": "#c998ff"}
    for j, name in enumerate(names):
        draw.text((218 + j * 220, 106), name.title(), font=font, fill=colors[name])
    draw.text((1100, 106), "Error along x=0..64 at trace row", font=font, fill="white")
    for row, (case, images) in enumerate(zip(record["cases"], frames)):
        top = 142 + row * 232
        probe = case["case"][0].replace("chromatic_", "").replace("neutral_", "neutral\n")
        draw.text((24, top), probe.replace("_", "\n", 1), font=font, fill="white", spacing=4)
        px, py = case["selected_pixel"]["active_local"]
        channel = case["selected_pixel"]["channel"]
        draw.text((24, top + 56), f"Trace ({px},{py}) {channel}\nOwn {case['selected_pixel']['observed_channel']}\nCrop {tuple(case['crop_active_local'][:2])}", font=font, fill="#bbc9d8", spacing=4)
        x, y, w, height = case["crop_active_local"]
        for j, name in enumerate(names):
            im = images[name]
            pixels = bytes(display_byte(v) for yy in range(y, y + height) for xx in range(x, x + w)
                           for v in im.pixels[(yy * im.width + xx) * 3:(yy * im.width + xx) * 3 + 3])
            crop = Image.frombytes("RGB", (w, height), pixels).resize((w * 8, height * 8), Image.Resampling.NEAREST)
            left = 218 + j * 220
            canvas.paste(crop, (left, top))
            mx, my = left + (px - x) * 8, top + (py - y) * 8
            draw.rectangle((mx, my, mx + 7, my + 7), outline="#ff39ad", width=2)
            if name != "truth":
                metric = case["frozen_metrics"][name][case["frozen_improvement_scope"]]["rgb"]["rmse"]
                scope = "Band" if case["frozen_improvement_scope"] == "edge_band" else "Interior"
                draw.text((left, top + 176), f"{scope} RGB RMSE {metric:.6f}", font=font, fill=colors[name])
            else:
                draw.text((left, top + 176), "Marker: selected pixel", font=font, fill="#bbc9d8")
        profile = case["profile"]["values"]
        errors = {name: [a - b for a, b in zip(profile[name], profile["truth"])] for name in names[1:]}
        bound = max(.0001, max(abs(v) for values in errors.values() for v in values)) * 1.1
        left, right, center_y, amplitude = 1100, 1410, top + 84, 72
        draw.line((left, center_y, right, center_y), fill="#67798a")
        for name, values in errors.items():
            points = [(left + i * (right - left) / (len(values) - 1), center_y - v * amplitude / bound) for i, v in enumerate(values)]
            draw.line(points, fill=colors[name], width=2)
        tx = left + px * (right - left) / (len(profile["truth"]) - 1)
        draw.line((tx, top, tx, top + 168), fill="#ff39ad", width=1)
        draw.text((left, top + 176), f"{channel} error / y={py}; +/-{bound:.4g}", font=font, fill="#bbc9d8")
    draw.text((24, 1300), "Worst excess-error pixels explain individual failures; these crops do not replace the full 2,368-case study or real-camera coverage.", font=font, fill="#bbc9d8")
    import io
    encoded = io.BytesIO()
    canvas.save(encoded, format="PNG")
    path = Path(path)
    payload = encoded.getvalue()
    if path.exists() and path.read_bytes() != payload:
        raise ValueError("refusing to overwrite different diagnostic figure")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return h.digest(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("study", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--figure", type=Path, help="optional Pillow-rendered crop/error figure")
    args = parser.parse_args()
    record, frames = evaluate(h.read_baseline(args.baseline), read_study(args.study))
    m.write_report(args.output, record)
    if args.figure:
        print(json.dumps({"figure_sha256": publish_figure(args.figure, record, frames)}, sort_keys=True))
    print(json.dumps({"case_count": len(record["cases"]), "native_replacement_accepted": False,
                      "report_sha256": h.digest(h.report_json(record).encode())}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
