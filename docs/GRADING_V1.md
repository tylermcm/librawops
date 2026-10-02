# Bounded lift/gain/signed-gamma grading v1

Status: frozen original contract before native implementation.

This adds creative per-channel grading in actual scene-linear ProPhoto/D50 or
Rec.2020/D65 RGB. It is an original explicitly ordered operator, not a claim of
matching another editor's lift/gamma/gain controls, exposure or white balance.

`GradingSettings`: RGB arrays `lift` (finite [-1,1], default0), `gain` (finite
[0,4], default1), `gamma` (finite [.25,4], default1). Copy settings/input into
immutable `GradingNode`. Validate every finite sample and exact tile storage,
bounds and descriptor before bypass. For each channel:

```text
if lift==0 and gain==1 and gamma==1: copy original float32 bits
scaled = input if gain==1 else double(input)*gain
u = scaled if lift==0 else scaled+lift
y = u if gamma==1 else copysign(pow(abs(u),1/gamma),u)
check y is finite and fits float32; cast once
```

No contraction, clipping, normalization, epsilon, gamut mapping or implicit
transfer conversion. Signed power is defined at zero with its intermediate sign.
Identity components preserve signed zeros, subnormals and extreme finite values;
nonidentity controls may underflow on cast. Genuine output overflow rejects.
Equal controls preserve numeric neutral equality; asymmetric controls can tint
neutrals/black. Each component's map is monotone nondecreasing for the allowed
gain range, continuous at zero but not necessarily differentiable there.
Gamma uses the platform binary64 math library; cross-toolchain bit-exact pow is
not promised. Independent high-precision truth uses a declared float32 tolerance.

Saved `rawengine.grading`, schema1/processing2, has exactly three three-number
arrays `lift`, `gain`, `gamma`; reject bools, missing/extra/shape/range/nonfinite
values and wrong versions even disabled. Enabled operation uses one image input,
matching supported domains, normal blend, opacity1, no masks/extensions. No halo
or geometry change. Native final/preview and preview mip1/2 map requested upstream
float32 samples. Nonlinear grading after reduction differs from grading before
reduction; preserve that order in cache/history/source-footprint contracts.

Verify independent Fraction affine and Decimal signed-power truth, channel/full
identity bits, neutral equality, monotonicity/continuity, negative/headroom,
black lift, boundaries/subnormals, overflow, malformed producers/settings/domains,
requested-level order, graph/cache/jobs/latest/history/analysis/geometry/working
conversion/mixed-source ownership. Run full default/core/LittleCMS builds/tests,
install checks and existing camera reference/partition/identity/visual/performance
gates. Production photographic/profile/corpus/Adobe qualification remains open;
personal aesthetic references do not define or block implementation. UI unchanged.
