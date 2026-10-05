#pragma once

#include "RgbaGraph.hpp"
#include <functional>

namespace rawengine {
RAWENGINE_API void validate_rgba_render_request(const RgbaNode& output, RenderRequest request);
class RAWENGINE_API RgbaRenderer final {
public:
    using TileCallback = std::function<void(const PremultipliedRgbaTile&)>;
    void render_tiles(const RgbaNode& output, RenderRequest request,
                      const TileCallback& callback, const CancellationToken* cancellation = nullptr) const;
    PremultipliedRgbaTile render_image(const RgbaNode& output, RenderRequest request,
                                      const CancellationToken* cancellation = nullptr) const;
};
} // namespace rawengine
