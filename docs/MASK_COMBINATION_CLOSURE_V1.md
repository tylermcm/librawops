# Original mask-combination requirement closure v1

2026-10-04. The original `Implement add/subtract/intersect/invert mask combination.`
row is complete. This audit joins the accepted [native graph evidence](MASK_GRAPH_EVIDENCE_V1.md),
[scalar delivery/Python evidence](COVERAGE_DELIVERY_EVIDENCE_V1.md), and current
[range/mixed-guide/full/installed acceptance](RANGE_MASK_EVIDENCE_V1.md).
No engine source, numeric contract, saved schema or installed binary changed.

| Required operation | Native map and actual saved dispatch |
|---|---|
| Invert | 1-a; CoverageInvertNode; rawengine.mask_invert |
| Add | min(1,a+b); CoverageCombineNode(Add); rawengine.mask_combine mode add |
| Subtract | max(0,a-b); CoverageCombineNode(Subtract); rawengine.mask_combine mode subtract |
| Intersect | a*b; CoverageCombineNode(Intersect); rawengine.mask_combine mode intersect |

The original independent 324 scalar fixtures, strict native actual-tile guards,
typed graph binding/signatures, generation/budget/owned cache returns, source-ID
footprints, mixed RGB consumers, histories and installed C++ tests remain current.
Full Release93/53/54/94 results retain byte-exact bound sources/binaries/logs.
The current installed mask graph consumer executes in both fresh default and
LCMS+Python prefixes. Earlier evidence remains historical and is not rewritten.

Supplemental [closure test](../tests/python_mask_combination_closure_tests.py)
replays all324 frozen cases through actual saved scalar graph operations in both
document working-space contexts (coverage itself has no working-space encoding).
At mip1/2 it independently reduces each input with the accepted Fraction oracle
before applying the selected algebra, retaining the frozen reduce-first policy.
Native Final/Preview and reduced Preview,1/2/8 partitions, every cell ROI, warm
cache, exact source-ID footprints, scalar jobs, history/undo/redo/restore, disabled
base/mask alias and invalid enabled/disabled mode types all pass. The supplemental
test is optional closure verification, separate from the existing CTest suites;
it runs against current default/LCMS+Python and both installed prefixes. Installed
engine/module path proofs use the accepted explicit-loading runner.

Immutable supplemental receipt
`build-msvc-release/research/mask-combination-closure-v2/verification.json`
SHA256 `49d9cfaec7466987e07ad3568c43c6e8264f4165ab8d28a8818647db8f223ce3`. The failedv1 log is preserved: its initial warm-cache assertion
compared a newly requested full9x9 tile against previously cached1/2/8 partitions.
The test now warms the identical request before checking reuse. Engine behavior
and expected pixels were correct; no cache fix was made. Closure archival bytes
for the preceding range handoff/receipt remain in
`research/before-mask-combination-closure-v1/` under build-msvc-release.

Fixed original checklist149 checked/131 open/280 total; exactly this existing
implementation row is newly closed. Broader mask/feather/alpha/layer quality and
asset/history-persistence/signoff rows remain open. Performance fixes including
PERF045 large-DAG support revisits remain deferred. Next freeze and implement
feather, density and edge-aware refinement. No UI or agent commit/push.
