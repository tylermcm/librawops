# Brush numeric primitive API addition v1

2026-10-04, frozen before the C++ brush kernel. This additive API clarification
keeps the [brush contract](BRUSH_MASK_CONTRACT_V1.md)'s controls, geometry,
arithmetic, ownership, graph and acceptance rules unchanged.

`BrushMask.hpp` additionally exports:

```cpp
float apply_brush_mask(float coverage, double center_x, double center_y,
                      const BrushMaskSettings& settings);
```

This evaluates the frozen ordered brush map at an explicit native-coordinate
sample center. It validates the complete settings and finite `[0,1]` input
coverage before evaluation. Center coordinates must be finite in `[-2^33,2^33]`.
It returns the frozen binary32 result and retains no caller storage. Empty or
zero-strength strokes preserve the input bits after validation. Integer pixel
centers used by CoverageBrushNode remain `(x+0.5,y+0.5)`.

The primitive supports direct numerical reuse and comparison of all independent
scalar fixtures, including negative/off-canvas or non-integer sample centers.
It introduces no extra raster sampling, clipping or coordinate transformation.
The node may call a private already-validated equivalent inside its pixel loop;
complete settings validation occurs at node admission, and complete actual
input-tile validation still occurs before mapping.
