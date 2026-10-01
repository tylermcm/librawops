"""Original symbolic source-support proof for the Menon research contract v1.

This evaluates sets of Bayer coordinates, not pixels. Both direction branches
and their classifiers contribute to the conservative support. No upstream code
or third-party runtime is imported. See RAW_DEMOSAIC_CANDIDATES.md for equations,
precision and the deliberate final-output border variant.
"""
import argparse
from dataclasses import dataclass
import hashlib
import itertools
import json
from pathlib import Path


PATTERNS = {"RGGB": (0, 1, 1, 2), "BGGR": (2, 1, 1, 0),
            "GRBG": (1, 0, 2, 1), "GBRG": (1, 2, 0, 1)}
AXES = ((1, 0), (0, 1))
# (dx, dy, weight): forward two-step edges contained in a centered 5x5.
H_EDGES = ((-2, -2, 1), (0, -2, 1), (-1, -1, 1),
           (-2, 0, 3), (0, 0, 3), (-1, 1, 1),
           (-2, 2, 1), (0, 2, 1))
V_EDGES = tuple(sorted(((dy, dx, w) for dx, dy, w in H_EDGES),
                       key=lambda v: (v[1], v[0])))
EXPECTED_RADII = {"directional_green": 2, "decision": 4,
                  "selected_green": 4, "color_at_green": 5,
                  "base_opposite_color": 6, "refined_green": 6,
                  "refined_color_at_green": 7, "refined_opposite_color": 8}


def integer(value, name):
    if type(value) is not int:
        raise ValueError(f"{name} must be an integer")
    return value


def point(value):
    if not isinstance(value, tuple) or len(value) != 2:
        raise ValueError("point must be an (x, y) tuple")
    return tuple(integer(v, "coordinate") for v in value)


def shifted(p, axis, step):
    return p[0] + axis[0] * step, p[1] + axis[1] * step


def radius(support, p):
    return max((max(abs(x - p[0]), abs(y - p[1])) for x, y in support), default=0)


@dataclass(frozen=True)
class Bounds:
    x: int
    y: int
    width: int
    height: int

    def __post_init__(self):
        for name in ("x", "y", "width", "height"):
            integer(getattr(self, name), name)
        if self.x < 0 or self.y < 0 or self.width < 1 or self.height < 1:
            raise ValueError("active bounds require nonnegative origin and positive extents")
        if self.width * self.height > 4096:
            raise ValueError("symbolic study is limited to 4096 active pixels")

    def contains(self, p):
        return self.x <= p[0] < self.x + self.width and self.y <= p[1] < self.y + self.height

    def margin(self, p):
        return min(p[0] - self.x, p[1] - self.y,
                   self.x + self.width - 1 - p[0], self.y + self.height - 1 - p[1])

    def points(self):
        return itertools.product(range(self.x, self.x + self.width),
                                 range(self.y, self.y + self.height))


class Support:
    """CFA geometry with per-instance, disposable support-set cache."""
    def __init__(self, pattern="RGGB", phase=(0, 0)):
        if not isinstance(pattern, str) or pattern not in PATTERNS:
            raise ValueError("unknown Bayer pattern")
        phase = point(phase)
        if any(v not in (0, 1) for v in phase):
            raise ValueError("phase must contain 0 or 1")
        self.pattern, self.phase = pattern, phase
        self._cache = {}

    def channel(self, p):
        x, y = point(p)
        return PATTERNS[self.pattern][((y + self.phase[1]) & 1) * 2 + ((x + self.phase[0]) & 1)]

    def axis_for_color(self, p, c):
        if self.channel(p) != 1 or c not in (0, 2):
            raise ValueError("nearest-color axis requires green site and red/blue target")
        return next(axis for axis in AXES if self.channel(shifted(p, axis, 1)) == c)

    def _memo(self, key, evaluate):
        if key not in self._cache:
            self._cache[key] = frozenset(evaluate())
        return self._cache[key]

    def directional_green(self, p, axis):
        if axis not in AXES:
            raise ValueError("unknown direction")
        if self.channel(p) == 1:
            return frozenset((p,))
        return frozenset(shifted(p, axis, n) for n in (-2, -1, 0, 1, 2))

    def decision(self, p):
        if self.channel(p) == 1:
            raise ValueError("direction decision is defined only at red/blue sites")
        def evaluate():
            leaves = {p}
            for axis, edges in zip(AXES, (H_EDGES, V_EDGES)):
                for dx, dy, _ in edges:
                    q = (p[0] + dx, p[1] + dy)
                    for endpoint in (q, shifted(q, axis, 2)):
                        leaves.update(self.directional_green(endpoint, axis))
            return leaves
        return self._memo(("decision", p), evaluate)

    def green(self, p):
        if self.channel(p) == 1:
            return frozenset((p,))
        return self._memo(("green", p), lambda: self.decision(p).union(
            *(self.directional_green(p, axis) for axis in AXES)))

    def color_at_green(self, p, c):
        axis = self.axis_for_color(p, c)
        return self._memo(("at_green", p, c), lambda: self.green(p).union(
            *(self.green(q) | {q} for q in (shifted(p, axis, -1), shifted(p, axis, 1)))))

    def base(self, p, c):
        own = self.channel(p)
        if c == own:
            return frozenset((p,))
        if c == 1:
            return self.green(p)
        if own == 1:
            return self.color_at_green(p, c)
        def evaluate():
            leaves = set(self.decision(p)) | {p}
            for axis in AXES:
                for n in (-1, 1):
                    q = shifted(p, axis, n)
                    leaves.update(self.color_at_green(q, c))
                    leaves.update(self.color_at_green(q, own))
            return leaves
        return self._memo(("base", p, c), evaluate)

    def refined_green(self, p):
        own = self.channel(p)
        if own == 1:
            return frozenset((p,))
        def evaluate():
            leaves = set(self.decision(p)) | {p}
            for axis in AXES:
                for n in (-1, 0, 1):
                    q = shifted(p, axis, n)
                    leaves.update(self.base(q, own))
                    leaves.update(self.base(q, 1))
            return leaves
        return self._memo(("refined_green", p), evaluate)

    def after_green_locations(self, p, c):
        own = self.channel(p)
        if c == own:
            return frozenset((p,))
        if c == 1:
            return self.refined_green(p)
        if own != 1:
            return self.base(p, c)
        axis = self.axis_for_color(p, c)
        return self._memo(("refined_at_green", p, c), lambda: self.refined_green(p).union(
            *(self.refined_green(q) | {q} for q in (shifted(p, axis, -1), shifted(p, axis, 1)))))

    def refined(self, p, c):
        own = self.channel(p)
        if c == own or c == 1 or own == 1:
            return self.after_green_locations(p, c)
        def evaluate():
            leaves = set(self.decision(p)) | {p}
            for axis in AXES:
                for n in (-1, 0, 1):
                    q = shifted(p, axis, n)
                    leaves.update(self.after_green_locations(q, c))
                    leaves.update(self.after_green_locations(q, own))
            return leaves
        return self._memo(("refined", p, c), evaluate)

    def uses_fallback(self, bounds, p, refine):
        point(p)
        if not isinstance(bounds, Bounds) or not bounds.contains(p):
            raise ValueError("output point is outside active bounds")
        if type(refine) is not bool:
            raise ValueError("refine must be boolean")
        return bounds.margin(p) < (8 if refine else 6)

    def output(self, bounds, p, c, refine=True):
        fallback = self.uses_fallback(bounds, p, refine)
        if type(c) is not int or c not in (0, 1, 2):
            raise ValueError("channel must be 0, 1 or 2")
        if c == self.channel(p):
            return frozenset((p,))
        if fallback:
            return frozenset(q for dx, dy in itertools.product((-1, 0, 1), repeat=2)
                             if bounds.contains(q := (p[0] + dx, p[1] + dy)) and self.channel(q) == c)
        return self.refined(p, c) if refine else self.base(p, c)


def make_report():
    measured = {name: set() for name in EXPECTED_RADII}
    witnesses = {}
    parity_cases = 0
    for pattern, px, py in itertools.product(PATTERNS, (0, 1), (0, 1)):
        study = Support(pattern, (px, py))
        for p in itertools.product((0, 1), repeat=2):
            own = study.channel(p)
            stages = {}
            if own != 1:
                c = 2 if own == 0 else 0
                stages = {"directional_green": study.directional_green(p, AXES[0]),
                          "decision": study.decision(p), "selected_green": study.green(p),
                          "base_opposite_color": study.base(p, c),
                          "refined_green": study.refined_green(p),
                          "refined_opposite_color": study.refined(p, c)}
            else:
                for c in (0, 2):
                    stages.setdefault("color_at_green", frozenset())
                    stages["color_at_green"] |= study.color_at_green(p, c)
                    stages.setdefault("refined_color_at_green", frozenset())
                    stages["refined_color_at_green"] |= study.after_green_locations(p, c)
            for name, leaves in stages.items():
                r = radius(leaves, p)
                measured[name].add(r)
                far = sorted((x - p[0], y - p[1]) for x, y in leaves
                             if max(abs(x - p[0]), abs(y - p[1])) == r)
                witnesses.setdefault(name, {"pattern": pattern, "phase": [px, py],
                                            "site": list(p), "radius": r,
                                            "outer_offsets": [list(v) for v in far]})
            parity_cases += 1
    if any(measured[name] != {expected} for name, expected in EXPECTED_RADII.items()):
        raise ValueError(f"stage support disagrees with contract: {measured}")

    outputs, interiors, layouts = 0, {"base": 0, "refined": 0}, 0
    for pattern, px, py, origin, extent in itertools.product(
            PATTERNS, (0, 1), (0, 1), ((0, 0), (3, 5)),
            ((1, 1), (1, 19), (19, 1), (12, 19), (13, 13), (16, 19), (17, 17), (21, 19))):
        bounds = Bounds(*origin, *extent)
        study = Support(pattern, (px, py))
        for p, refine in itertools.product(bounds.points(), (False, True)):
            if not study.uses_fallback(bounds, p, refine):
                interiors["refined" if refine else "base"] += 1
            for c in range(3):
                leaves = study.output(bounds, p, c, refine)
                if not all(bounds.contains(q) for q in leaves):
                    raise ValueError("output reads inactive samples")
                outputs += 1
        layouts += 1
    source = Path(__file__).read_bytes().replace(b"\r\n", b"\n")
    return {"schema_version": 1, "contract": "librawops-menon-support-v1",
            "kind": "symbolic-dependency-proof", "evaluates_pixels": False,
            "native_replacement_accepted": False,
            "source_sha256_lf": hashlib.sha256(source).hexdigest(),
            "primary_paper_sha256": "4f207dffd4529b7df0453e71bbe958d311dc9a1619dfb8c6c2e200d3c6ecffbd",
            "stage_radii": dict(EXPECTED_RADII), "outer_witnesses": witnesses,
            "cfa_phase_site_cases": parity_cases, "active_layout_cases": layouts,
            "channel_output_checks": outputs, "interior_point_variant_checks": interiors,
            "active_confinement_passed": True,
            "halo": {"base": 6, "refined": 8},
            "scope": "All period-two CFA sites/branches; bounded finite active layouts. No pixel/quality/native proof."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = make_report()
    payload = (json.dumps(report, sort_keys=True, indent=2) + "\n").encode("utf-8")
    if args.output.exists() and args.output.read_bytes() != payload:
        raise ValueError("refusing to overwrite different proof evidence")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    print(f"Support proof passed: base halo 6, refined halo 8; {report['channel_output_checks']} active checks")


if __name__ == "__main__":
    main()
