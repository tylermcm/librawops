# Paused coverage delivery work — 2026-10-04

Paused at the user's request before bed. The persistent development goal is
paused; resume only when the user asks. Feature implementation remains first,
with performance investigations/fixes deferred and observations recorded in
PERFORMANCE_ISSUES.md. The viewer remains testing-only and unpackageable.

Last accepted checkpoint: MASK_GRAPH_EVIDENCE_V1.md. Full Release suites passed
82 default / 46 core / 47 LittleCMS / 83 LittleCMS+Python, with fresh default/LCMS
installed mask/source/alpha consumers. The immutable final receipt is
build-msvc-release/research/mask-graph-final-checkpoint-v1.json, SHA256
6fb957c7fc0e8def0f656f67e1e2f4acf13ea4a907d6c56a1ceec7fb409fc220.
Checklist remains 145 checked / 135 open / 280 total. Log has 38 entries through
PERF-039; the next new issue ID is PERF-040.

## Unverified work since that acceptance

- Froze the proposed bounded scalar delivery/Python session contract in
  COVERAGE_DELIVERY_CONTRACT_V1.md; no acceptance claim yet.
- Preserved 19 exact pre-delivery artifacts, including current graph/scheduler/
  Python/CMake/handoff files and four accepted DLLs plus Python modules, under
  build-msvc-release/research/before-coverage-delivery-v1, with bindings.json.
- Added CoverageDelivery.hpp/.cpp: scalar request validation, row-major tile
  streaming, actual-tile validation, image assembly and cooperative cancellation.
- Modified TileScheduler.hpp/.cpp: typed coverage futures sharing existing
  workers, budget, priority/FIFO and cross-payload supersession groups. Queue
  admission is now shared internally; RGB future API is preserved in source.
- These edits have NOT been compiled or tested. CoverageDelivery.cpp/header
  have NOT been wired into CMake build/install yet. Python source admission,
  explicit scalar session/history methods and typed jobs are NOT implemented.
  Graphify is stale relative to these latest edits and must be updated.
- Existing DLLs still represent the accepted mask graph checkpoint. Do not
  present them as evidence for the new scheduler/header implementation.

## Resume actions

Wire the new delivery module into CMake/install, compile and resolve functional
issues, then implement Python coverage source ownership in RasterGraphSession.
Use explicit coverage render/submit/history methods from the frozen contract;
existing RGB methods must reject scalar output. Add independent scalar delivery
and promise-gated mixed-queue tests, then Python source/manifest/cache/history/job
ownership tests. Run the required regression configurations and fresh installed
consumers before accepting this gate. Preserve old receipts and use fresh report
versions. Update Graphify and the canonical plan/handoff after acceptance.

No commit/push or UI work was performed. Brush/parametric/range/feather masks,
RGBA graph/layers and binary asset persistence remain original-plan work after
this delivery/session gate. Camera NR remains deferred.
