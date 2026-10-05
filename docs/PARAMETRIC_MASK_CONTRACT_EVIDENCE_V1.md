# Linear/radial/polygon pre-native contract evidence

2026-10-04. [Frozen contract](PARAMETRIC_MASK_CONTRACT_V1.md) specifies three
geometric mask types: linear ramp, forward-axis radial ellipse and even-odd polygon
rings. Shape is stored to binary32, optionally inverted with existing mask algebra,
then multiplied with a complete validated native input mask. Nodes replay native
Final before direct preview reduction and retain copied settings.

[Independent Fraction/IEEE-stage oracle](../tests/reference/parametric_mask_oracle.py)
and [frozen header](../tests/reference/parametric_mask_v1.hpp) pass **988 staged
shape/input-product cases, 36 native/mip frames and 996 independently replayed
cell ROIs**. No engine imports, native floating math/casts, NumPy or imaging
dependency is used. Existing exact IEEE encoder and direct coverage reduction
oracle remain byte unchanged.

Cases include endpoint plateaus/reversed/oblique/minimum/maximum ramps,
elliptical rotated/oblique/reflected/hard/soft axes, tiny/large geometry,
convex/concave/hole/self-crossing/repeated/collinear rings, edge/vertex samples,
inversion after float32 storage, signed-zero endpoint copy, coverage subnormals,
off-canvas and shifted/extreme origins. Hand-derived anchors verify projection,
radial transition/circumference, round ellipse coordinates, hole parity/boundary,
concave exclusion and nearest-even underflow. Short/singular/inverse-amplification
admission examples reject. Selected dyadic ring reversals preserve parity; no
universal floating reversal/resampling equality is claimed. All three canonical
saved operation JSON strings and independent SHA256 digests are frozen.

This is pre-native contract/prototype evidence, **not implementation acceptance**.
Native primitives/nodes, strict saved tree/typed graph binding, actual request/tile
guards, ROI/tile/mip parity, caller ownership, cache/source-ID/history/jobs/Python
and installed use remain to implement and verify. The original linear/radial/
parametric implementation row stays open. Checklist: 146 checked / 134 open /
280 total, unchanged. Ranges/refinement/RGBA/layers/assets/qualification remain
required later work. Camera NR deferred; no UI or agent commit/push.

Before implementation, verified all current brush full/installed/final receipt
bindings and retained an exact accepted-artifact snapshot at
`build-msvc-release/research/before-parametric-mask-v1/bindings.json`.
Prior accepted numeric sources/fixtures and four native DLLs/two current cp39
modules are preserved. Existing brush acceptance remains authoritative for its
historical builds; use fresh versions for native implementation verification.

```bat
build-msvc-release\research\run-with-windows-env.cmd C:\Users\tylle\anaconda3\python.exe tests\reference\parametric_mask_oracle.py --check
```

Next implement ParametricMask.hpp/.cpp with strict staged-FP compiler flags, then
the three format-4 schema-1/process-2 scalar graph types and complete native/Python/
installed acceptance. Keep cheap observations in the performance log; defer
dedicated performance investigation/fixes until the feature build is complete.
