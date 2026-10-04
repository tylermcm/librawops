# Python ICC/profile bridge v1

Status: API and ownership boundary frozen before implementation, 2026-10-03.
This completes an existing Phase1 Python-profile/Phase4 integration requirement;
it does not redefine photographic quality, monitor discovery, file readers,
gamut mapping or release qualification. The library remains decoder-free and
the development viewer remains outside packaging.

## Profiles and availability

`icc_available()` reports whether this extension was compiled with the existing
optional core-only LittleCMS backend. Backend-off builds retain the same
non-ICC behavior and expose the functions; profile creation raises RuntimeError.
No runtime backend selection, automatic profile discovery or new dependency.

`create_icc_profile(profile_bytes, *, intent='relative_colorimetric',
black_point_compensation=False)` returns an opaque immutable owned profile
handle. `icc_profile_info(handle)` returns a fresh dict containing exact
`profile_sha256`, `intent`, `black_point_compensation`, `engine`, `engine_version`
and `profile_bytes` length. No caller-supplied digest/policy replacement.

Profile bytes are copied from a contiguous one-dimensional byte buffer before
releasing the GIL. Accept 128 bytes through 16 MiB inclusive, matching the native
backend. Validate the existing RGB display/output ICC class/header policy and
construct the native transform before publishing a handle. Accept only the
four exact native intent names: perceptual, relative_colorimetric, saturation,
absolute_colorimetric. BPC must be bool. Reject wrong handles/types, malformed,
unsupported/non-RGB profiles, invalid policies, noncontiguous buffers and sizes
outside the bound. Bytes, not a filename, are the input; no live external asset.

Existing LittleCMS identity/version and hard output clip remain unchanged.
Handle creation/copies may hold two profile-byte vectors (bridge and backend),
each bounded at16 MiB; the backend also owns its context/transform. This is a
logical bound, not an allocator/process-peak claim. Importing a source owns
another exact profile copy and uint16 source samples as the native adapter does.
Profile reuse does not repeatedly parse the output profile for each tile.

## Source and output APIs

Existing `RasterSession` accepts keyword-only `input_profile=None` and
`output_profile=None`. With input_profile, rgb explicitly means profile-encoded
native-endian interleaved RGB uint16; without it, the existing float32 input
contract remains unchanged. Working space stays mandatory. Stride includes
padding; exact storage count is stride*height*3, with existing uint32/size bounds.
ICC buffers must be native uint16 or raw contiguous bytes of the exact size;
reject nonnative element formats. Source construction retains copied samples,
profile bytes, declared working space and exact native ICC policy identity.

`RasterGraphSession` source specs additionally accept `input_profile`; atomic
replacement uses the same validated owned-source path. Mixed ICC/scene-linear
sources remain explicitly typed. Native source footprints and independent
source/profile/demosaic signatures remain as the C++ graph declares.

`RawSession` and `RasterGraphSession` also accept keyword-only output_profile.
The configured output transform is immutable for the lifetime of a session.
It is available to ICC graphs and is not applied to non-ICC graphs. Requests
cannot replace source/profile/session configuration. Saved manifests bind their
own output profile/policy to that exact configured transform; mismatches fail.

`output_mode='icc-display'` selects the explicit working-to-linear-sRGB, tone,
ICC output chain and requires an output profile. RAW additionally requires the
existing explicit camera calibration. `render` and `render_raster` one-shot
options accept output_profile; render_raster also accepts input_profile with the
same explicit uint16 semantics. A one-shot output_profile without icc-display
is rejected. Legacy/sRGB defaults and existing versions are unchanged.

ICC input sources and ICC output remain native Final only; rendering, jobs,
analysis and footprint planning must reject unsupported reduced/quality requests.
No encoded-source averaging or invented profile-aware reduced preview.

## Graph, cache, jobs and history

Recipe exports and saved graphs contain the existing input/output profile
identities, exact source fingerprint, working domain and processing/schema
versions. Profile byte payloads are not embedded in manifest/history JSON;
restore requires matching caller-supplied owned sources and output profile.

Cache keys use existing profile/policy/source/operation/level identities.
Changing samples, input profile, intent or BPC cannot reuse stale output under a
saved identity. Jobs pin native source/transform nodes independently of Python
handle lifetime. Existing clear/cancel/close/supersession behavior is preserved.
Native work and transform creation release the GIL after copying Python data.

History retains the configured transform as available rendering state. Each
revision passes it to graph construction only when that revision declares ICC
output. This allows ICC and non-ICC revisions in one history without applying
the profile to non-ICC states. Direct ExecutableEditGraph unused-transform
validation remains unchanged. Saved restore, comparison and jobs use pinned
sources/profile availability and validate each ICC revision's exact policy.

## Acceptance

Backend-off default regression and explicit unavailable behavior; optional
LittleCMS+Python build and installed-module smoke; original C++/Python suites;
four native intent/BPC policy combinations and digest/byte ownership; padded
uint16 import in both spaces; Python/direct native parity with actual backend;
ROI/tile/signed-headroom output; source/profile/policy mismatch and malformed
input; cache reuse/invalidation; sync/async/latest/analysis/source footprints;
RAW explicit calibration; mixed multi-source replacement; ICC/non-ICC history
navigation/comparison/restore and jobs after handle/session deletion; native-only
level gates. Independent native reference fixtures must use exact supplied
profile bytes, not inferred monitor settings. Retain complete logs and measured
profile/source construction costs with scope limitations under existing PERF IDs.

This boundary does not claim independent ICC color accuracy or representative
profile/camera quality. Those existing plan gates stay open.
