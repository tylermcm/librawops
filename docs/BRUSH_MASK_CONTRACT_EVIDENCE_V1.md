# Brush mask pre-native contract evidence

2026-10-04. [Frozen contract](BRUSH_MASK_CONTRACT_V1.md) specifies immutable
paint/erase stroke ownership, round polyline geometry, pressure/flow/opacity,
ordered staged arithmetic, native replay before direct mip reduction and strict
format-4 scalar graph/cache/source-ID/history/Python integration requirements.

[Independent Fraction oracle](../tests/reference/brush_mask_oracle.py) and
[frozen fixtures](../tests/reference/brush_mask_v1.hpp) verify **426 staged scalar
cases, 12 native/mip image frames and 332 independently replayed cell ROIs**.
The oracle imports no engine or imaging package and uses exact rational IEEE
rounding at each binary64/binary32 stage. Existing IEEE encoder and source
reduction oracles are reused unchanged. Hand anchors prove center/soft transition/
circumference, round caps, pressure interpolation, same-stroke duplicate
idempotence, separate-stroke accumulation/order, underflow rounding and no-op
signed-zero bit preservation. Bounds and native-to-direct-mip cell checks pass.
Canonical saved operation JSON and its independently computed SHA256 are frozen.

This is contract/prototype evidence, **not native implementation acceptance**.
Settings/request/actual-tile/graph guards, caller mutation isolation, full/ROI/tile
bit parity, cache/source-ID/history/jobs/Python and installed use remain to verify
after implementation. Original reusable-brush row remains open; fixed checklist
stays 145 checked / 135 open / 280 total.

The preceding scalar delivery full/installed/final receipts and their exact
current bound artifacts were checked before any brush implementation. A fresh
byte-for-byte archive is retained at
`build-msvc-release/research/before-brush-mask-v1/bindings.json`.
Previous successful receipts remain immutable and refer to historical binaries.

```bat
build-msvc-release\research\run-with-windows-env.cmd C:\Users\tylle\anaconda3\python.exe tests\reference\brush_mask_oracle.py --check
```

Continue implementing BrushMask, then strict saved graph integration and the
full acceptance gates. Performance fixes remain deferred; no brush UI required.
