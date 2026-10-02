# Scene-linear grayscale / monochrome v1

Status: **functional fixed working-Y conversion; creative-look qualification separate**.

This is parameter-free conversion to the existing working-space luminance Y,
replicated into RGB. It retains the scene-linear ProPhoto/D50 or Rec.2020/D65
descriptor and RGB storage. It does not introduce a single-channel raster format,
creative channel weights, toning, clipping, gamma or photographic look.
Creative repeated-row mixing remains available through ChannelMixerNode.

Export `GrayscaleNode(std::shared_ptr<const Node>)` in ToneOps.hpp. Own the input;
require its actual descriptor to match one of the two supported working spaces.
Validate requested tile bounds, exact storage, descriptor and every finite
float32 sample before neutral bypass. Null inputs and camera/encoded domains
reject. Finite neutral triples, including numeric equality with mixed signed
zeros, copy every original channel bit. Other triples use the same pinned
binary64 Y coefficients and no-contraction arithmetic as saturation/color balance:

| Working space | wr | wb |
| --- | --- | --- |
| ProPhoto/D50 | 0.28807112822929337 | 0.00008565396060525903 |
| Rec.2020/D65 | 0.26270021201126703 | 0.059301716469861945 |

```text
Y = (G + wr * (R - G)) + wb * (B - G)
gray = float32(Y)
output = [gray, gray, gray]
```

Check finite float32 representability before the single cast. Negative values
and over-one headroom survive. The exact mathematical coefficients form a
positive partition; do not claim bit-exact Y after float32 rounding, equality
with saturation(0)'s different arithmetic, or an unreachable bounded overflow
fixture. Already neutral inputs retain their bits, so repeated conversion is
idempotent. Nonneutral channels are bit-identical after mapping.

Saved type `rawengine.grayscale`, schema1/processing2, has exactly `{}` parameters.
Reject extras, wrong container and wrong versions even disabled. Enabled graphs
require one image input, matching supported input/output domains, normal blend,
opacity1 and no masks/extensions. Disabled behavior uses the existing passthrough.
Cache identity includes type/version/upstream/domain/requested level.

Native final/preview and preview mip1/2 map the requested upstream float32 pixels.
This adds no halo or geometry; preserve source footprints. Mathematical linear
conversion commutes with averaging, but casting/reduction order need not produce
the same bits. Existing graph/cache/jobs/latest/history/analysis, working-space
conversion and multiple-source APIs apply without new Python entry points.
Default graphs, decoder, UI, dependencies and prior algorithms remain unchanged.

Qualification: independent exact-rational dot-Y on chromatic/signed/headroom/
subnormal/zero-Y/extreme inputs in both spaces; neutral/signed-zero exact bits,
idempotence and equal output channels; malformed producers, domains and requests;
strict saved type and disabled versions; graph/mip/ROI/tile/geometry/conversion/
multi-source/cache/jobs/history/analysis/ownership; full default/core/LittleCMS
builds and installed public API/exports/notices. Existing camera ROIs need
independent references, exact partitions/disabled identity and unchanged historical
inputs, inspected display boards and bounded API/memory measurements after tests.
Production photographic looks, qualified profiles, Adobe comparison and wider
full-frame/concurrency/allocator evidence remain separate gates. UI is a local
test harness excluded from library installation.

## Verification — 2026-10-02

Native controls and seven independent exact-rational/Python integration tests
pass; full rebuilt Release **48/48 default,27/27 core,28/28 LittleCMS** passes.
Neutral/sign/extreme bits,idempotence/equal output channels,strict errors and
requested-level/geometry/ROI/cache/jobs/history/analysis/conversion/multi-source
ownership verified. Installed public API/binaries/exports/two notices checked;
unchanged viewer9/9 and adapter10/10 compatibility tests pass.

48 fixed camera references match independent NumPy float64 dot-Y exactly;
all partitions/disabled identities exact,twelve historical calibrated inputs
unchanged,eight before/gray boards inspected. Maximum rounded output-Y residual
1.48043097e-08. Eight native timing cases/seven repeats,256-square/64 MiB cache:
warm-input uncached/cached API median case medians **1.194650/0.503450 ms**. PERF-021 records
scope and remaining gaps,not isolated kernel/full-frame/regression evidence.

Ignored report `tests/rawfiles/_librawops_local/high-iso/grayscale-verified-v1/grayscale-camera-check-v1.json`,
SHA256 `c3dd0bc7234e6a985f4ac374cfcbece540a79510c1dd8f57cbc74892e0835cd5`, binds sources/native/helper/decoded/ROI/runtime/boards.
Frozen pre-native contract and binaries archived in before-grayscale/.
Creative photographic looks/profile/Adobe qualification stay separate.
See [handoff](plan/LIBRAWOPS_PLAN.md) and
[remaining color-control gates](COLOR_CONTROL_QUALIFICATION.md).
