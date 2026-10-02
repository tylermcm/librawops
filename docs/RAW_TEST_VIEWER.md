# Simple local RAW test viewer

This is a local development test harness only. UI changes are limited to what
is needed to test or inspect engine behavior. The viewer, launcher and its
GUI/decoder runtimes will **not be packaged or distributed alongside the library**.

Double-click `tools/launch_raw_viewer.cmd`. Click **Open NEF…** to select a file.
The launcher uses the existing home engine Python and external decoder Python;
no packages are installed. This optional test tool is separate from the engine
installation and never changes the original file.

The plain Tk window has **Adjustments** and **Color mixer** tabs and provides:

- **Fit** and **100%**. Double-click a point in Fit to inspect it at native
  resolution; drag the 100% view to pan. Native detail renders only the viewport.
- **Bilinear / Menon** source reconstruction.
- Exposure in stops and red/blue WB multipliers relative to as-shot WB; green
  stays at its normalized as-shot value.
- A shared RGB three-knot curve midpoint and affine levels black/white endpoints.
- **Saturation** from 0 (grayscale) to 4, with 1 as exact identity. The native
  working-space operation runs after levels/curves, before display conversion.
- **Vibrance** from -1 to 1, with 0 as exact identity. This original adaptive
  chroma operation follows saturation; it has no skin-protection promise and
  -1 does not promise grayscale.
- **Color mixer**: select one of eight hue bands and adjust Hue (degrees),
  Chroma delta and Luminance delta. Every band retains separate values; all
  default to zero. The original scene-linear mixer runs after vibrance.
  Luminance fades near neutrals; these are native working-RGB controls.
- **Before (as-shot)** bypasses all adjustments in both tabs using the selected
  demosaicer. **Reset adjustments** restores identity; it retains the selected
  demosaicer, hue-band selection and view.
- Current image dimensions and native render time; first opening also reports
  decoder/session/render time. Errors appear in the bottom status line.

## Run with explicit runtimes

From the repository root in PowerShell:

```powershell
& "$env:USERPROFILE\anaconda3\python.exe" tools/raw_test_viewer.py `
  --module-dir build-msvc-release `
  --decoder-python "$env:USERPROFILE\OneDrive\Documents\Playground\.msi_build_venv\Scripts\python.exe" `
  tests/rawfiles/_DSC1793.NEF
```

The engine Python must match the built extension (currently home Python 3.9).
It needs Tk, NumPy and Pillow. The existing separate Python 3.13 decoder needs
rawpy, NumPy and Pillow. If one runtime supplies everything, pass that same
Python as `--decoder-python`. Override the double-click launcher's paths with
`RAW_VIEWER_PYTHON` and `RAW_VIEWER_DECODER_PYTHON` environment variables if needed.

## Behavior and limits

The decoder process opens/unpacks supported Nikon RGB Bayer NEFs into a unique
temporary directory; it does not postprocess or render the photograph. Native
RawSession receives copied uint16 sensor data and site metadata. Temporary
decoded files are removed after loading. A single background worker owns the
source and lazily creates sessions for each selected demosaicer, at 64 MiB cache
per session. It retains one pending view, cancels obsolete native jobs at tile
boundaries and rejects stale results. Closing cancels native work and kills an
in-progress decoder process; decode timeout is 90 seconds. No worker calls Tk.

The explicit saved graph uses as-shot WB, exposure, the existing diagnostic
decoder-derived camera matrix, active-area crop/EXIF orientation, scene-linear
levels/curves/saturation/vibrance/color mixer, then the existing tone/sRGB output chain. All eight EXIF transforms
are supported. Fit uses mip2 before resizing the encoded view for the window;
100% uses mip0 final with exact native pixel display. Requested-level point-edit
semantics apply, so nonlinear curves/vibrance/color mixer can differ between Fit and native detail.

This is local visual testing. Camera profiles, monitor management, decoder
distribution, production control behavior and export remain separate open gates.
No denoise is applied. The initial ISO20000 sample will show substantial grain.
The default window is 1180×820; its minimum 820×780 keeps the controls visible.

## Verification — 2026-10-02, color mixer

Nine viewer tests and ten camera-adapter tests pass. Native code/binaries are
unchanged;preceding full44/44 default,25/25 core,26/26 LittleCMS remains latest.
The mixer follows vibrance and precedes display;all-zero defaults preserve
the original image bytes. Before bypasses all basic controls and all24 band
values;Reset clears both tabs while retaining the band,demosaicer and view.

Ignored `build-msvc-release/research/viewer-color-mixer-smoke.py` verifies19
actual-window states on `_DSC1793.NEF`,each exactly matching a separately owned
native session. Includes retained multiple bands/all eight selectors/three
sliders,Before/restoration/reset,Fit/native,both demosaicers,minimum tabs,
rapid-edit coalescing and opening-file reset. Eight final captures in
`tests/rawfiles/_librawops_local/viewer-color-mixer-v2/` inspected. Navigation
hint,controls and status remain visible at820×780 after correcting spacing
found in the initial capture;initial v1 helper/evidence retained separately.
Decoder-error/recovery/pending-detail/close checks pass;two opening-worker
closes110/16 ms. No worker remains after verification.

One workflow:open 2480.7 ms,Bilinear Fit 1145.5–1303.4 ms/native
68.0–82.2 ms,Menon Fit 2473.1–2738.5 ms/native 112.0–161.0 ms.
PERF-012 records full-preview cache churn and profiling points;this expanded
graph/reference workflow is not controlled regression timing or a kernel benchmark.
Final report `viewer-color-mixer-smoke-v1.json`,SHA256
`5a3ccc135c32e056b1a79a93157c9651dcbf375be73e26e5b6c047354a65a821`,binds UI/adapter/decoder/tests/helper,
current DLL/pyd,decoded input,19 states/cache counters/eight captures.
Prior sources preserved in `build-msvc-release/before-color-mixer-viewer/`;
earlier captures/native archives remain historical evidence.

## Historical verification — 2026-10-02, vibrance

Channel-mixer engine compatibility follow-up: unchanged viewer sources pass
eight optional controls and ten camera adapter tests against the newer native
build (full38/38 default,22/22 core,23/23 LittleCMS). The window/capture evidence
below binds the pre-mixer DLL, archived in `before-channel-mixer/`; the channel
mixer itself is currently available through C++/saved manifests.

Eight optional viewer tests and ten camera adapter tests pass. Full rebuilt native
CTest passes 36/36 default, 21/21 core-only, 22/22 LittleCMS; additional zero-Y
vibrance controls pass after test-only updates. The saved graph retains original
identity output bytes through saturation1/vibrance0.

`build-msvc-release/research/viewer-vibrance-smoke.py` exercises 14 actual-window
states on `_DSC1793.NEF`, each exactly matching a separately owned native session:
identity/negative/positive vibrance, Before/reset, Fit/native detail, both
demosaicers, minimum geometry and rapid-edit coalescing. All six captures in
ignored `tests/rawfiles/_librawops_local/viewer-vibrance-v1/` were inspected;
controls/tips/status remain visible at minimum size. Decoder error/recovery and
closing during opening pass; two closes took 0/31 ms in this run.

One workflow observed open+render2.499 s, Bilinear Fit1.120–1.214 s/native61.0–76.4
ms, Menon native109.4–135.6 ms/Fit2.432–2.479 s. Each full pass adds702 misses/zero
hits at64 MiB. PERF-012 retains the ongoing cache-churn observation and deferred
improvement points; these expanded-graph/reference-workflow timings are not a
controlled regression. `viewer-vibrance-smoke-v1.json` SHA-256
`238ff44fe7d095d523be98ea33283876fbd824d877eeddcaef9570713612f047`
binds four current UI/adapter/test sources, current native DLL, decoded input,
14 state hashes/cache counters and six captures. Earlier sources/native binaries
are represented by their separate historical reports and archived binaries.

## Historical verification — 2026-10-02, saturation

Seven optional viewer tests and ten camera adapter tests pass. Focused saturation
CTest passes 2/2. Native source/binaries were unchanged this viewer run; the
previous full native matrix remains 34/34 default, 20/20 core-only, 21/21 LittleCMS.

`build-msvc-release/research/viewer-saturation-smoke.py` exercises 14 actual-window
states on `_DSC1793.NEF`, each exactly matching a separately owned native session:
identity/grayscale/1.75 saturation, Before/reset, Fit/native detail, both
demosaicers, minimum size and rapid-edit coalescing. Initial identity also retains
the pre-saturation viewer output hash. Six desktop captures were inspected in
ignored `tests/rawfiles/_librawops_local/viewer-saturation-v1/`; controls, tips and
status fit at minimum size. Decoder error/recovery/close lifecycle checks pass.

One workflow observed opening+render 2.922 s, bilinear Fit 1.093–1.159 s/native
detail 55.6–64.6 ms, Menon detail 107.4–122.2 ms and Fit 2.461–2.574 s.
Six full bilinear passes each add 648 misses/zero hits at 64 MiB; PERF-012 records
the continuing cache churn. These are workflow costs with an expanded graph and
reference checks, not a controlled regression or isolated saturation benchmark.

`viewer-saturation-smoke-v1.json` SHA-256
`65cc13aaaab0602882f0521065402df38f8a35a5bd71dbce1be8cacc48a7016d`
binds four current source files, decoded input, unchanged native DLL, state output
hashes/cache counters and six capture hashes. Earlier evidence remains separate.

## Historical verification — 2026-10-01

`python tests/raw_viewer_tests.py build-msvc-release` passes six optional adapter
tests: independent EXIF/native pixel truth with nonzero active origin, Before
and identity against the original recipe, native ROI clipping, coalesced/stale
requests, close during load, and error recovery. Native code/binaries were
unchanged; prior full Release CTest 32/32 default, 19/19 core-only and 20/20
LittleCMS was the native matrix at that checkpoint.

The ignored `build-msvc-release/research/viewer-smoke.py` exercised nine actual
Tk window states on `_DSC1793.NEF`: Fit, edits, exact Before/reset, double-click
native detail, drag-to-pan, Menon selection, rapid-edit coalescing and return to
Fit. Five actual desktop captures were inspected under ignored
`tests/rawfiles/_librawops_local/viewer-final-v2/`. Opening plus rendering took
2.341 s; bilinear Fit requests 1.044–1.116 s, native detail 49–52 ms, Menon native
detail 102–126 ms and Menon Fit 2.106 s. These are single workflow observations on the
home i9-14900K/default runtime, not regression or kernel benchmarks. PERF-012
in the authoritative plan records the full-preview cache churn and improvement
points. Decoded source storage, two lazy sessions, caches, copied RGB results
and display images are not one globally bounded memory budget.

An additional actual-window lifecycle check showed a missing-file decoder error
without a stale image, recovery by opening `_DSC0636.NEF`, and safe switching to
100% while the previous Fit result is still displayed. Closing at the start of
decoding terminated the worker immediately or within 47 ms in two observed runs.

Final smoke report SHA-256
`1883fdfeb5de74cecee0fcad8497368967c21234cbb406d511d9e9ed778da4f0`
binds the three adapter/UI source files and unchanged native DLL. Earlier smoke
reports/captures remain under separate ignored directories.
