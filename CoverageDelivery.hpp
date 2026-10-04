#pragma once

#include "MaskOps.hpp"
#include <functional>

namespace rawengine {

// Validates scalar extent/level/viewport/tile-size without rendering.
RAWENGINE_API void validate_coverage_render_request(const CoverageNode& output, RenderRequest request);

class RAWENGINE_API CoverageRenderer final {
public:
    using TileCallback = std::function<void(const CoverageTile&)>;
    void render_tiles(const CoverageNode& output, RenderRequest request,
                      const TileCallback& callback, const CancellationToken* cancellation = nullptr) const;
    CoverageTile render_image(const CoverageNode& output, RenderRequest request,
                              const CancellationToken* cancellation = nullptr) const;
};

} // namespace rawengine
