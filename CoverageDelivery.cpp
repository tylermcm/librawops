#include "CoverageDelivery.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rawengine {

void validate_coverage_render_request(const CoverageNode& output, RenderRequest request) {
    const auto extent=output.output_bounds(request.level);
    const auto roi=request.viewport;
    if (!request.tile_size || !extent.width || !extent.height ||
        roi.x<extent.x || roi.y<extent.y ||
        static_cast<std::uint64_t>(roi.x)+roi.width>static_cast<std::uint64_t>(extent.x)+extent.width ||
        static_cast<std::uint64_t>(roi.y)+roi.height>static_cast<std::uint64_t>(extent.y)+extent.height)
        throw std::invalid_argument("invalid coverage render request");
}

void CoverageRenderer::render_tiles(const CoverageNode& output, RenderRequest request,
                                    const TileCallback& callback,const CancellationToken* cancellation) const {
    validate_coverage_render_request(output,request);
    if (!callback) throw std::invalid_argument("coverage tile callback is required");
    auto cancelled=[&] {if (cancellation && cancellation->is_cancelled()) throw RenderCancelled();};
    cancelled();
    const auto right=static_cast<std::uint64_t>(request.viewport.x)+request.viewport.width;
    const auto bottom=static_cast<std::uint64_t>(request.viewport.y)+request.viewport.height;
    for (std::uint64_t y=request.viewport.y;y<bottom;y+=request.tile_size)
        for (std::uint64_t x=request.viewport.x;x<right;x+=request.tile_size) {
            cancelled();
            const Rect bounds{static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(request.tile_size,right-x)),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(request.tile_size,bottom-y))};
            const auto tile=output.render_level(bounds,request.level);
            if (tile.bounds.x!=bounds.x || tile.bounds.y!=bounds.y ||
                tile.bounds.width!=bounds.width || tile.bounds.height!=bounds.height)
                throw std::domain_error("coverage node returned wrong delivery ROI");
            validate_coverage_tile(tile);
            callback(tile);
        }
    cancelled();
}

CoverageTile CoverageRenderer::render_image(const CoverageNode& output,RenderRequest request,
                                           const CancellationToken* cancellation) const {
    validate_coverage_render_request(output,request);
    if (cancellation && cancellation->is_cancelled()) throw RenderCancelled();
    const auto count=static_cast<std::uint64_t>(request.viewport.width)*request.viewport.height;
    if (count>std::numeric_limits<std::size_t>::max()/sizeof(float) ||
        count>std::vector<float>().max_size()) throw std::length_error("coverage image exceeds storage capacity");
    CoverageTile image{request.viewport,std::vector<float>(static_cast<std::size_t>(count))};
    render_tiles(output,request,[&](const CoverageTile& tile) {
        for (std::uint32_t row=0;row<tile.bounds.height;++row) {
            const auto src=static_cast<std::size_t>(row)*tile.bounds.width;
            const auto dst=static_cast<std::size_t>(tile.bounds.y-request.viewport.y+row)*request.viewport.width+
                           tile.bounds.x-request.viewport.x;
            std::memcpy(image.coverage.data()+dst,tile.coverage.data()+src,
                        static_cast<std::size_t>(tile.bounds.width)*sizeof(float));
        }
    },cancellation);
    return image;
}

} // namespace rawengine
