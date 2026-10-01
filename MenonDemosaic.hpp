#pragma once
#include "RawEngine.hpp"

namespace rawengine::detail {
// Internal implementation; public selection is RawDemosaicIdentity.
// Caller validates the request and routes singleton axes to native bilinear.
Tile render_menon_base(const RawImage& image, Rect request);
}
