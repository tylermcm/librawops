# Range-mask pre-native contract evidence v1

2026-10-04. Frozen [contract](RANGE_MASK_CONTRACT_V1.md), independent
[Fraction oracle](../tests/reference/range_mask_oracle.py) and generated
[bit fixtures](../tests/reference/range_mask_v1.hpp) define original luminance,
RGB ellipsoid and supplied normalized-depth selection. Native implementation is
absent; the existing original implementation checkbox remains open.

The oracle verifies 12,236 stored selection/input-product cases, 30 native/direct
mip frames and 830 independently traversed native/reduced cells. Cases cover both
working spaces, neutral/signed/headroom/full finite exponent RGB, scalar soft/hard/
singleton endpoints, color centers/scales/soft/hard boundaries, inversion after
binary32 storage and mask endpoints/signed zero/subnormals. Hand-derived anchors
check scalar ramps/plateaus/singletons, neutral Y, ellipsoid center/axis symmetry,
soft versus hard circumference and nearest-even subnormal products. Twenty-one
invalid setting/guide probes reject. These are oracle guards, not native guards.

Three explicit order witnesses yield native reduced selection zero but selection
one after averaging the guide first: luminance endpoints 0/1, color axis samples
-1/+1, and depth endpoints 0/1. This proves the required native-before-mip order.
Three canonical saved operation byte strings and independent SHA256 values are
pinned in the header. Future native serialization must reproduce them exactly.

The accepted geometric milestone's current sources, binaries, fixtures, docs and
receipts were verified before archiving 56 byte-exact artifacts under
`build-msvc-release/research/before-range-mask-v1/`. The unchanged current default
native Python module rejects all three planned types enabled and disabled in both
working spaces: 12 actual unsupported-operation rejections. This establishes a
pre-native boundary, not successful execution. Current accepted Release results
remain 90/51/52/91; they were not rerun for these research/docs-only additions.

Local immutable provenance: `research/range-mask-pre-native-checkpoint-v1.json`
binds the candidate contract, oracle/header, accepted receipts and rejection text.
Its exact candidate contract bytes are retained separately as
`research/range-mask-contract-candidate-v1.md`. The frozen acceptance checkpoint
is `research/range-mask-contract-checkpoint-v1.json` (all research paths are relative
to `build-msvc-release/`). Configured-host installed evidence remains the accepted
geometric milestone's evidence, not range installation or clean-machine certification.

Next implement RangeMask.hpp/.cpp, strict saved types and scalar-to-RGB dependency
planning, then replay frozen native/Python/ROI/cache/history/jobs gates and fresh
installed consumers. Complete both-input actual validation even for zero masks;
preserve disabled guide typing while excluding its sampling footprint. Broader
feather/refinement, RGBA/layers, assets, quality, performance and release remain.
Fixed original checklist 147 checked/133 open/280 total. No UI, algorithm benchmark,
performance fix, agent commit or push.
