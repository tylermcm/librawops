#include "RgbaDelivery.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rawengine {

void validate_rgba_render_request(const RgbaNode& output, RenderRequest request) {
    const bool native=request.level.mip==0 &&
        (request.level.quality==RenderQuality::Final || request.level.quality==RenderQuality::Preview);
    const bool reduced=(request.level.mip==1 || request.level.mip==2) && request.level.quality==RenderQuality::Preview;
    if ((!native && !reduced) || !output.supports_level(request.level))
        throw std::invalid_argument("unsupported coverage delivery level");
    const auto extent=output.output_bounds(request.level);
    if (output.working_space()!=WorkingSpace::LinearProPhotoD50 && output.working_space()!=WorkingSpace::LinearRec2020D65)
        throw std::invalid_argument("RGBA delivery working space is unsupported");
    const auto roi=request.viewport;
    if (!request.tile_size || !extent.width || !extent.height ||
        static_cast<std::uint64_t>(extent.x)+extent.width>static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())+1 ||
        static_cast<std::uint64_t>(extent.y)+extent.height>static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())+1 ||
        roi.x<extent.x || roi.y<extent.y ||
        static_cast<std::uint64_t>(roi.x)+roi.width>static_cast<std::uint64_t>(extent.x)+extent.width ||
        static_cast<std::uint64_t>(roi.y)+roi.height>static_cast<std::uint64_t>(extent.y)+extent.height)
        throw std::invalid_argument("invalid coverage render request");
}

void RgbaRenderer::render_tiles(const RgbaNode& output, RenderRequest request,
                                    const TileCallback& callback,const CancellationToken* cancellation) const {
    validate_rgba_render_request(output,request);
    if (!callback) throw std::invalid_argument("coverage tile callback is required");
    auto cancelled=[&] {if (cancellation && cancellation->is_cancelled()) throw RenderCancelled();};
    cancelled();
    const auto space=output.working_space();
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
                tile.bounds.width!=bounds.width || tile.bounds.height!=bounds.height || tile.working_space!=space)
                throw std::domain_error("coverage node returned wrong delivery ROI");
            validate_premultiplied_rgba_tile(tile);
            callback(tile);
        }
    cancelled();
}

PremultipliedRgbaTile RgbaRenderer::render_image(const RgbaNode& output,RenderRequest request,
                                           const CancellationToken* cancellation) const {
    validate_rgba_render_request(output,request);
    if (cancellation && cancellation->is_cancelled()) throw RenderCancelled();
    const auto count=static_cast<std::uint64_t>(request.viewport.width)*request.viewport.height;
    if (count>std::numeric_limits<std::size_t>::max()/sizeof(float)/4 ||
        count>std::vector<float>().max_size()/4) throw std::length_error("coverage image exceeds storage capacity");
    PremultipliedRgbaTile image{request.viewport,std::vector<float>(static_cast<std::size_t>(count)*4),output.working_space()};
    render_tiles(output,request,[&](const PremultipliedRgbaTile& tile) {
        for (std::uint32_t row=0;row<tile.bounds.height;++row) {
            const auto src=static_cast<std::size_t>(row)*tile.bounds.width;
            const auto dst=static_cast<std::size_t>(tile.bounds.y-request.viewport.y+row)*request.viewport.width+
                           tile.bounds.x-request.viewport.x;
            std::memcpy(image.rgba.data()+dst*4,tile.rgba.data()+src*4,
                        static_cast<std::size_t>(tile.bounds.width)*4*sizeof(float));
        }
    },cancellation);
    return image;
}

} // namespace rawengine
