# Guided-filter scheduler recovery

This opt-in public C++ consumer injects one-shot faults into a diagnostic source
feeding the guided-filter node. The installed engine remains unchanged.

```powershell
cmake -S bench/guided_filter_qualification/recovery -B build-guided-recovery -G Ninja -DCMAKE_BUILD_TYPE=Release -DRAWENGINE_INSTALL_PREFIX=C:/absolute/engine-install
cmake --build build-guided-recovery
$env:PATH = "C:/absolute/engine-install/bin;" + $env:PATH
build-guided-recovery/GuidedFilterRecovery.exe C:/absolute/evidence/recovery-v1.json
```

The output path must be new. Windows reports the actual loaded DLL; verify it
against the intended installation. The current accepted run used MSVC19.40
Release and the same installed DLL as the resource studies. Other configurations
remain unqualified by this consumer.

The65×49 signed/headroom raster covers both working spaces, radii0/3/8, native
Final/Preview and mip1/2 Preview. Five faults occur at either the first or second
source request: allocation exception, backend exception, short RGB storage,
incorrect actual tile descriptor, or NaN input. A promise blocks the sole worker
at the fault boundary until a healthy request using the same node is queued.
There are240 injected failures,240 queued recoveries and240 later recoveries.
Recovered complete images must match the unfaulted direct render byte for byte,
using different4/8/16 tile sizes. The original exception category must reach the
failed future. Gate waits have10-second timeouts.

The48 allocation exceptions are explicit `std::bad_alloc` throws at the source
boundary. This does not inject real allocator failures inside filter coefficients,
output assembly, cache, job scheduling or history. It also does not trace allocation
ownership or certify leak behavior, latest-group handling, cache failure recovery,
or whole-engine memory budgets. Those build-plan requirements remain open.
