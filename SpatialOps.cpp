#include "SpatialOps.hpp"
#include "EditGraph.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rawengine {
namespace {
bool same_rect(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
void bounded_rect(Rect r) {
    if (!r.width || !r.height || std::uint64_t(r.x) + r.width > (std::uint64_t{1} << 32) ||
        std::uint64_t(r.y) + r.height > (std::uint64_t{1} << 32))
        throw std::invalid_argument("spatial operation requires nonempty addressable bounds");
}
void contained(Rect r, Rect image) {
    bounded_rect(r); bounded_rect(image);
    if (r.x < image.x || r.y < image.y || std::uint64_t(r.x) + r.width > std::uint64_t(image.x) + image.width ||
        std::uint64_t(r.y) + r.height > std::uint64_t(image.y) + image.height)
        throw std::out_of_range("spatial viewport is outside image bounds");
}
Rect level_bounds(Rect native, RenderLevel level) {
    bounded_rect(native);
    if (level.mip == 0 && (level.quality == RenderQuality::Final || level.quality == RenderQuality::Preview)) return native;
    if (level.mip < 1 || level.mip > 2 || level.quality != RenderQuality::Preview)
        throw std::invalid_argument("unsupported spatial render level");
    const auto scale = 1u << level.mip;
    return {0,0,native.width / scale + (native.width % scale != 0),native.height / scale + (native.height % scale != 0)};
}
Rect expanded(Rect r, Rect image, std::uint32_t rx, std::uint32_t ry) {
    contained(r,image);
    const auto left = std::max<std::int64_t>(image.x, std::int64_t(r.x) - rx);
    const auto top = std::max<std::int64_t>(image.y, std::int64_t(r.y) - ry);
    const auto right = std::min(std::uint64_t(image.x)+image.width,std::uint64_t(r.x)+r.width+rx);
    const auto bottom = std::min(std::uint64_t(image.y)+image.height,std::uint64_t(r.y)+r.height+ry);
    return {static_cast<std::uint32_t>(left),static_cast<std::uint32_t>(top),
            static_cast<std::uint32_t>(right-left),static_cast<std::uint32_t>(bottom-top)};
}
template<class T> std::size_t elements(std::uint32_t w, std::uint32_t h) {
    if (w && h > std::vector<T>().max_size() / w / 3)
        throw std::length_error("spatial tile exceeds addressable storage");
    return std::size_t(w)*h*3;
}
void valid_tile(const Tile& tile, Rect needed, ImageDescriptor descriptor) {
    if (!same_rect(tile.bounds,needed) || tile.rgb.size()!=elements<float>(needed.width,needed.height) ||
        tile.descriptor != descriptor || descriptor.format != PixelFormat::RGBFloat32)
        throw std::invalid_argument("spatial input tile bounds, storage or descriptor differ");
}
struct Moment {
    std::uint32_t count = 0;
    double mean = 0, m2 = 0;
    void add(float value) {
        if (!std::isfinite(value)) return;
        ++count;
        const double delta = double(value)-mean;
        mean += delta/count;
        m2 += delta*(double(value)-mean);
    }
    void merge(const Moment& b) {
        if (!b.count) return;
        if (!count) { *this=b; return; }
        const auto total = count+b.count;
        const double delta = b.mean-mean;
        m2 += b.m2 + delta*delta*(double(count)*b.count/total);
        mean += delta*(double(b.count)/total);
        count = total;
    }
};
} // namespace

Rect local_statistics_region(Rect output, Rect image_bounds, std::uint32_t radius) {
    if (radius > 8) throw std::invalid_argument("local statistics radius must be in 0..8");
    return expanded(output,image_bounds,radius,radius);
}

LocalStatisticsTile local_statistics_rgb(const Tile& input, Rect output, Rect image_bounds, std::uint32_t radius) {
    const auto needed = local_statistics_region(output,image_bounds,radius);
    valid_tile(input,needed,input.descriptor);
    const auto count = elements<double>(output.width,output.height);
    LocalStatisticsTile result{output,input.descriptor,radius,
        std::vector<double>(count),std::vector<double>(count),std::vector<std::uint32_t>(count)};
    // Independent fixed-order horizontal moments and vertical merges avoid
    // subtracting nearly equal E[x^2] and E[x]^2. Complete halos preserve the
    // same arithmetic order for each window across render tile partitions.
    std::vector<Moment> horizontal(elements<Moment>(output.width,needed.height));
    for (std::uint32_t row=0; row<needed.height; ++row) {
        for (std::uint32_t col=0; col<output.width; ++col) {
            const auto x=std::uint64_t(output.x)+col;
            const auto left=std::max<std::int64_t>(image_bounds.x,std::int64_t(x)-radius);
            const auto right=std::min(std::uint64_t(image_bounds.x)+image_bounds.width-1,x+radius);
            const auto target=(std::size_t(row)*output.width+col)*3;
            for (auto xx=std::uint64_t(left); xx<=right; ++xx) {
                const auto source=(std::size_t(row)*needed.width+std::size_t(xx-needed.x))*3;
                for (unsigned c=0;c<3;++c) horizontal[target+c].add(input.rgb[source+c]);
            }
        }
    }
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    for (std::uint32_t row=0; row<output.height; ++row) {
        const auto y=std::uint64_t(output.y)+row;
        const auto top=std::max<std::int64_t>(image_bounds.y,std::int64_t(y)-radius);
        const auto bottom=std::min(std::uint64_t(image_bounds.y)+image_bounds.height-1,y+radius);
        for (std::uint32_t col=0; col<output.width; ++col) {
            Moment total[3];
            for (auto yy=std::uint64_t(top); yy<=bottom; ++yy) {
                const auto source=(std::size_t(yy-needed.y)*output.width+col)*3;
                for (unsigned c=0;c<3;++c) total[c].merge(horizontal[source+c]);
            }
            const auto target=(std::size_t(row)*output.width+col)*3;
            for (unsigned c=0;c<3;++c) {
                result.finite_count[target+c]=total[c].count;
                result.mean[target+c]=total[c].count ? total[c].mean : nan;
                result.variance[target+c]=total[c].count ? std::max(0.0,total[c].m2/total[c].count) : nan;
            }
        }
    }
    return result;
}

void analyze_local_rgb(const Node& output, Rect native_bounds, const RenderRequest& request,
                       const LocalStatisticsCallback& callback, std::uint32_t radius,
                       const CancellationToken* cancellation) {
    const auto image=level_bounds(native_bounds,request.level);
    (void)local_statistics_region(request.viewport,image,radius);
    if (!callback || !request.tile_size || !output.supports_level(request.level))
        throw std::invalid_argument("local analysis needs a callback, positive tile size and supported level");
    auto check=[&] { if (cancellation && cancellation->is_cancelled()) throw RenderCancelled(); };
    check();
    const auto right=std::uint64_t(request.viewport.x)+request.viewport.width;
    const auto bottom=std::uint64_t(request.viewport.y)+request.viewport.height;
    for (auto y=std::uint64_t(request.viewport.y);y<bottom;y+=request.tile_size)
        for (auto x=std::uint64_t(request.viewport.x);x<right;x+=request.tile_size) {
            check();
            const Rect rect{static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(request.tile_size,right-x)),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(request.tile_size,bottom-y))};
            const auto needed=local_statistics_region(rect,image,radius);
            const auto input=output.render_level(needed,request.level);
            check();
            valid_tile(input,needed,output.output_descriptor());
            const auto result=local_statistics_rgb(input,rect,image,radius);
            check();
            callback(result);
            check();
        }
}
void analyze_local_rgb(const ImageGraph& graph, const RenderRequest& request,
                       const LocalStatisticsCallback& callback, std::uint32_t radius, const CancellationToken* cancellation) {
    analyze_local_rgb(graph.output(),graph.source_bounds(),request,callback,radius,cancellation);
}
void analyze_local_rgb(const ExecutableEditGraph& graph, const RenderRequest& request,
                       const LocalStatisticsCallback& callback, std::uint32_t radius, const CancellationToken* cancellation) {
    analyze_local_rgb(graph.output(),graph.output_bounds(),request,callback,radius,cancellation);
}

void validate_convolution_kernel(const ConvolutionKernel& kernel) {
    if (!kernel.width || !kernel.height || kernel.width>17 || kernel.height>17 ||
        kernel.width%2!=1 || kernel.height%2!=1 || kernel.coefficients.size()!=std::size_t(kernel.width)*kernel.height)
        throw std::invalid_argument("convolution kernel needs odd dimensions 1..17 and matching coefficients");
    for (double value:kernel.coefficients)
        if (!std::isfinite(value) || std::abs(value)>65536)
            throw std::invalid_argument("convolution coefficients must be finite with magnitude <=65536");
}
ConvolutionNode::ConvolutionNode(std::shared_ptr<const Node> input, Rect native_bounds, ConvolutionKernel kernel)
    : input_(std::move(input)),native_bounds_(native_bounds),kernel_(std::move(kernel)) {
    bounded_rect(native_bounds_); validate_convolution_kernel(kernel_);
    if (!input_ || (input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
                   input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)))
        throw std::invalid_argument("convolution requires scene-linear working RGB");
    identity_=true;
    const auto center=kernel_.coefficients.size()/2;
    for (std::size_t i=0;i<kernel_.coefficients.size();++i)
        if (kernel_.coefficients[i]!=(i==center ? 1.0 : 0.0)) identity_=false;
}
bool ConvolutionNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect ConvolutionNode::input_region(Rect output, Rect source_bounds) const {
    return input_region_level(output,source_bounds,{});
}
Rect ConvolutionNode::input_region_level(Rect output, Rect source_bounds, RenderLevel level) const {
    const auto image=level_bounds(native_bounds_,level);
    if (!same_rect(source_bounds,image) || !supports_level(level))
        throw std::invalid_argument("convolution source bounds or level differ");
    return expanded(output,image,identity_ ? 0 : kernel_.width/2,identity_ ? 0 : kernel_.height/2);
}
Tile ConvolutionNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile ConvolutionNode::render_level(Rect bounds, RenderLevel level) const {
    const auto image=level_bounds(native_bounds_,level);
    const auto needed=input_region_level(bounds,image,level);
    auto input=input_->render_level(needed,level);
    valid_tile(input,needed,output_descriptor());
    for (float value:input.rgb) if (!std::isfinite(value))
        throw std::invalid_argument("convolution requires finite input samples");
    if (identity_) return input;
    Tile result{bounds,std::vector<float>(elements<float>(bounds.width,bounds.height)),output_descriptor()};
    const auto right=std::int64_t(image.x)+image.width-1,bottom=std::int64_t(image.y)+image.height-1;
    for (std::uint32_t row=0;row<bounds.height;++row)
        for (std::uint32_t col=0;col<bounds.width;++col) {
            double sum[3]{};
            for (std::uint32_t ky=0;ky<kernel_.height;++ky)
                for (std::uint32_t kx=0;kx<kernel_.width;++kx) {
                    const auto x=std::clamp(std::int64_t(bounds.x)+col+kernel_.width/2-kx,std::int64_t(image.x),right);
                    const auto y=std::clamp(std::int64_t(bounds.y)+row+kernel_.height/2-ky,std::int64_t(image.y),bottom);
                    const auto source=(std::size_t(y-needed.y)*needed.width+std::size_t(x-needed.x))*3;
                    const auto weight=kernel_.coefficients[std::size_t(ky)*kernel_.width+kx];
                    for (unsigned c=0;c<3;++c) sum[c]+=weight*input.rgb[source+c];
                }
            const auto target=(std::size_t(row)*bounds.width+col)*3;
            for (unsigned c=0;c<3;++c) {
                if (!std::isfinite(sum[c]) || std::abs(sum[c])>std::numeric_limits<float>::max())
                    throw std::invalid_argument("convolution output exceeds finite float32 range");
                result.rgb[target+c]=static_cast<float>(sum[c]);
            }
        }
    return result;
}
} // namespace rawengine
