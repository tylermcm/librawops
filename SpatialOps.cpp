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
void validate_clarity_settings(const ClaritySettings& settings) {
    if (!std::isfinite(settings.amount) || std::abs(settings.amount)>1 ||
        settings.radius<1 || settings.radius>8)
        throw std::invalid_argument("clarity requires finite amount [-1,1] and radius 1..8");
}
ClarityNode::ClarityNode(std::shared_ptr<const Node> input, Rect native_bounds, ClaritySettings settings)
    : input_(std::move(input)), native_bounds_(native_bounds), settings_(settings) {
    bounded_rect(native_bounds_); validate_clarity_settings(settings_);
    if (!input_) throw std::invalid_argument("clarity requires input");
    if (input_->output_descriptor()==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50)) {
        weight_red_=0.28807112822929337; weight_blue_=0.00008565396060525903;
    } else if (input_->output_descriptor()==ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)) {
        weight_red_=0.26270021201126703; weight_blue_=0.059301716469861945;
    } else throw std::invalid_argument("clarity requires scene-linear working RGB");
}
bool ClarityNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect ClarityNode::input_region(Rect output, Rect source_bounds) const {
    return input_region_level(output,source_bounds,{});
}
Rect ClarityNode::input_region_level(Rect output, Rect source_bounds, RenderLevel level) const {
    const auto image=level_bounds(native_bounds_,level);
    if (!same_rect(source_bounds,image) || !supports_level(level))
        throw std::invalid_argument("clarity source bounds or level differ");
    return expanded(output,image,settings_.amount==0 ? 0 : settings_.radius,
        settings_.amount==0 ? 0 : settings_.radius);
}
Tile ClarityNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile ClarityNode::render_level(Rect bounds, RenderLevel level) const {
    const auto image=level_bounds(native_bounds_,level);
    const auto needed=input_region_level(bounds,image,level);
    auto input=input_->render_level(needed,level);
    valid_tile(input,needed,output_descriptor());
    for (float value:input.rgb) if (!std::isfinite(value))
        throw std::invalid_argument("clarity requires finite complete input halo");
    if (settings_.amount==0) return input;
    // PERF-024: optional Y cache avoids repeated formation without changing
    // expression or window sum order. Neighborhood/API/allocation cost needs measurement.
    std::vector<double> luminance(input.rgb.size()/3);
    for (std::size_t i=0;i<luminance.size();++i) {
        const double r=input.rgb[3*i],g=input.rgb[3*i+1],b=input.rgb[3*i+2];
        const double y=(g+weight_red_*(r-g))+weight_blue_*(b-g);
        if (!std::isfinite(y)) throw std::invalid_argument("clarity nonfinite working Y");
        luminance[i]=y;
    }
    Tile result{bounds,std::vector<float>(elements<float>(bounds.width,bounds.height)),output_descriptor()};
    for (std::uint32_t row=0;row<bounds.height;++row)
        for (std::uint32_t col=0;col<bounds.width;++col) {
            const auto x=std::uint64_t(bounds.x)+col,y=std::uint64_t(bounds.y)+row;
            const auto source=(std::size_t(y-needed.y)*needed.width+std::size_t(x-needed.x));
            const auto target=(std::size_t(row)*bounds.width+col)*3;
            const double center=luminance[source];
            double offset=0;
            if (center>0 && center<1) {
                const auto left=std::max<std::int64_t>(image.x,std::int64_t(x)-settings_.radius);
                const auto top=std::max<std::int64_t>(image.y,std::int64_t(y)-settings_.radius);
                const auto right=std::min(std::uint64_t(image.x)+image.width-1,x+settings_.radius);
                const auto bottom=std::min(std::uint64_t(image.y)+image.height-1,y+settings_.radius);
                double sum=0;
                for (auto yy=std::uint64_t(top);yy<=bottom;++yy)
                    for (auto xx=std::uint64_t(left);xx<=right;++xx) {
                        const double difference=luminance[std::size_t(yy-needed.y)*needed.width+std::size_t(xx-needed.x)]-center;
                        sum=sum+difference;
                        if (!std::isfinite(difference) || !std::isfinite(sum))
                            throw std::invalid_argument("clarity nonfinite neighborhood difference");
                    }
                const auto count=(right-std::uint64_t(left)+1)*(bottom-std::uint64_t(top)+1);
                const double detail=-(sum/static_cast<double>(count));
                if (!std::isfinite(detail)) throw std::invalid_argument("clarity nonfinite detail");
                if (detail!=0) {
                    const double weight=(4*center)*(1-center);
                    const double strength=settings_.amount*weight;
                    offset=strength*detail;
                    if (!std::isfinite(weight) || !std::isfinite(strength) || !std::isfinite(offset))
                        throw std::invalid_argument("clarity nonfinite offset");
                }
            }
            for (unsigned c=0;c<3;++c) {
                if (offset==0) result.rgb[target+c]=input.rgb[3*source+c];
                else {
                    const double mapped=double(input.rgb[3*source+c])+offset;
                    if (!std::isfinite(mapped) || std::abs(mapped)>std::numeric_limits<float>::max())
                        throw std::invalid_argument("clarity output exceeds finite float32 range");
                    result.rgb[target+c]=static_cast<float>(mapped);
                }
            }
        }
    return result;
}
void validate_texture_settings(const TextureSettings& settings) {
    if (!std::isfinite(settings.amount) || std::abs(settings.amount)>1 ||
        settings.scale<1 || settings.scale>4)
        throw std::invalid_argument("texture requires finite amount [-1,1] and scale 1..4");
}
TextureNode::TextureNode(std::shared_ptr<const Node> input, Rect native_bounds, TextureSettings settings)
    : input_(std::move(input)), native_bounds_(native_bounds), settings_(settings) {
    bounded_rect(native_bounds_); validate_texture_settings(settings_);
    if (!input_) throw std::invalid_argument("texture requires input");
    if (input_->output_descriptor()==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50)) {
        weight_red_=0.28807112822929337; weight_blue_=0.00008565396060525903;
    } else if (input_->output_descriptor()==ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)) {
        weight_red_=0.26270021201126703; weight_blue_=0.059301716469861945;
    } else throw std::invalid_argument("texture requires scene-linear working RGB");
}
bool TextureNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect TextureNode::input_region(Rect output, Rect source_bounds) const {
    return input_region_level(output,source_bounds,{});
}
Rect TextureNode::input_region_level(Rect output, Rect source_bounds, RenderLevel level) const {
    const auto image=level_bounds(native_bounds_,level);
    if (!same_rect(source_bounds,image) || !supports_level(level))
        throw std::invalid_argument("texture source bounds or level differ");
    const auto radius=settings_.amount==0 ? 0 : 2*settings_.scale;
    return expanded(output,image,radius,radius);
}
Tile TextureNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile TextureNode::render_level(Rect bounds, RenderLevel level) const {
    const auto image=level_bounds(native_bounds_,level);
    const auto needed=input_region_level(bounds,image,level);
    auto input=input_->render_level(needed,level);
    valid_tile(input,needed,output_descriptor());
    for (float value:input.rgb) if (!std::isfinite(value))
        throw std::invalid_argument("texture requires finite complete input halo");
    if (settings_.amount==0) return input;

    auto native_y=[&](std::size_t i) {
        const double r=input.rgb[3*i],g=input.rgb[3*i+1],b=input.rgb[3*i+2];
        const double value=(g+weight_red_*(r-g))+weight_blue_*(b-g);
        if (!std::isfinite(value)) throw std::invalid_argument("texture nonfinite working Y");
        return value;
    };
    const auto halo_count=elements<double>(needed.width,needed.height)/3;
    const auto output_count=elements<double>(bounds.width,bounds.height)/3;
    // PERF-025: two reusable halo planes and a saved fine-output plane retain
    // fixed pass order/true borders. API, allocation and axis-pass costs need measurement.
    std::vector<double> plane(halo_count),temporary(halo_count),fine(output_count);
    for (std::size_t i=0;i<halo_count;++i) plane[i]=native_y(i);
    auto index=[&](std::uint64_t x,std::uint64_t y) {
        return std::size_t(y-needed.y)*needed.width+std::size_t(x-needed.x);
    };
    auto axis=[&](const std::vector<double>& source,std::vector<double>& destination,Rect valid,bool vertical) {
        const auto lower=vertical ? std::uint64_t(image.y) : std::uint64_t(image.x);
        const auto upper=vertical ? std::uint64_t(image.y)+image.height : std::uint64_t(image.x)+image.width;
        const auto stride=vertical ? std::size_t(needed.width) : std::size_t{1};
        for (std::uint32_t row=0;row<valid.height;++row)
            for (std::uint32_t col=0;col<valid.width;++col) {
                const auto x=std::uint64_t(valid.x)+col,y=std::uint64_t(valid.y)+row;
                const auto i=index(x,y),coordinate=vertical ? y : x;
                const double center=source[i];
                double sum=0;
                unsigned weight=2;
                if (coordinate>lower) {
                    const double difference=source[i-stride]-center;
                    sum=sum+difference; ++weight;
                    if (!std::isfinite(difference) || !std::isfinite(sum))
                        throw std::invalid_argument("texture nonfinite negative-axis difference");
                }
                if (coordinate+1<upper) {
                    const double difference=source[i+stride]-center;
                    sum=sum+difference; ++weight;
                    if (!std::isfinite(difference) || !std::isfinite(sum))
                        throw std::invalid_argument("texture nonfinite positive-axis difference");
                }
                const double delta=sum/weight;
                const double value=delta==0 ? center : center+delta;
                if (!std::isfinite(delta) || !std::isfinite(value))
                    throw std::invalid_argument("texture nonfinite blur plane");
                destination[i]=value;
            }
    };
    for (std::uint32_t pass=1;pass<=2*settings_.scale;++pass) {
        const auto radius=2*settings_.scale-pass;
        const auto valid=expanded(bounds,image,radius,radius);
        const auto horizontal=expanded(valid,image,0,1);
        // Neighbor existence uses the true image, never a shrinking plane edge.
        // The previous plane contains this horizontal region plus its x halo.
        axis(plane,temporary,horizontal,false);
        axis(temporary,plane,valid,true);
        if (pass==settings_.scale)
            for (std::uint32_t row=0;row<bounds.height;++row)
                for (std::uint32_t col=0;col<bounds.width;++col)
                    fine[std::size_t(row)*bounds.width+col]=plane[index(std::uint64_t(bounds.x)+col,std::uint64_t(bounds.y)+row)];
    }
    Tile result{bounds,std::vector<float>(elements<float>(bounds.width,bounds.height)),output_descriptor()};
    for (std::uint32_t row=0;row<bounds.height;++row)
        for (std::uint32_t col=0;col<bounds.width;++col) {
            const auto source=index(std::uint64_t(bounds.x)+col,std::uint64_t(bounds.y)+row);
            const auto pixel=std::size_t(row)*bounds.width+col;
            const double center=native_y(source);
            double offset=0;
            if (center>0 && center<1) {
                const double detail=fine[pixel]-plane[source];
                if (!std::isfinite(detail)) throw std::invalid_argument("texture nonfinite detail");
                if (detail!=0) {
                    const double weight=(4*center)*(1-center);
                    const double strength=settings_.amount*weight;
                    offset=strength*detail;
                    if (!std::isfinite(weight) || !std::isfinite(strength) || !std::isfinite(offset))
                        throw std::invalid_argument("texture nonfinite offset");
                }
            }
            for (unsigned c=0;c<3;++c) {
                if (offset==0) result.rgb[3*pixel+c]=input.rgb[3*source+c];
                else {
                    const double mapped=double(input.rgb[3*source+c])+offset;
                    if (!std::isfinite(mapped) || std::abs(mapped)>std::numeric_limits<float>::max())
                        throw std::invalid_argument("texture output exceeds finite float32 range");
                    result.rgb[3*pixel+c]=static_cast<float>(mapped);
                }
            }
        }
    return result;
}
void validate_sharpen_settings(const SharpenSettings& s) {
    if (!std::isfinite(s.amount) || s.amount<0 || s.amount>2 || s.radius<1 || s.radius>3)
        throw std::invalid_argument("sharpen requires finite amount 0..2 and radius 1..3");
}
SharpenNode::SharpenNode(std::shared_ptr<const Node> input, Rect native_bounds, SharpenSettings settings)
    : input_(std::move(input)),native_bounds_(native_bounds),settings_(settings) {
    bounded_rect(native_bounds_);validate_sharpen_settings(settings_);
    if (!input_ || (input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)))
        throw std::invalid_argument("sharpen requires scene-linear working RGB");
}
bool SharpenNode::supports_level(RenderLevel l) const noexcept {
    return ((l.mip==0 && (l.quality==RenderQuality::Final || l.quality==RenderQuality::Preview)) ||
        (l.mip>=1 && l.mip<=2 && l.quality==RenderQuality::Preview)) && input_->supports_level(l);
}
Rect SharpenNode::input_region(Rect r, Rect image) const { return input_region_level(r,image,{}); }
Rect SharpenNode::input_region_level(Rect r, Rect image, RenderLevel l) const {
    if (!supports_level(l) || !same_rect(image,level_bounds(native_bounds_,l)))
        throw std::invalid_argument("sharpen source bounds or render level differ");
    return expanded(r,image,settings_.amount==0 ? 0 : settings_.radius,settings_.amount==0 ? 0 : settings_.radius);
}
Tile SharpenNode::render(Rect r) const { return render_level(r,{}); }
Tile SharpenNode::render_level(Rect r, RenderLevel l) const {
    const auto image=level_bounds(native_bounds_,l),needed=input_region_level(r,image,l);
    auto input=input_->render_level(needed,l);valid_tile(input,needed,output_descriptor());
    for (float x:input.rgb) if (!std::isfinite(x))
        throw std::invalid_argument("sharpen requires finite complete RGB halo");
    if (settings_.amount==0) return input;
    Tile result{r,std::vector<float>(elements<float>(r.width,r.height)),output_descriptor()};
    // PERF-027: share RGB address traversal while retaining each channel's
    // ordered centered sum; only scalar centers/sums, no double image planes.
    for (std::uint32_t row=0;row<r.height;++row) for (std::uint32_t col=0;col<r.width;++col) {
        const auto x=std::uint64_t(r.x)+col,y=std::uint64_t(r.y)+row;
        const auto left=std::max<std::int64_t>(image.x,std::int64_t(x)-settings_.radius);
        const auto top=std::max<std::int64_t>(image.y,std::int64_t(y)-settings_.radius);
        const auto right=std::min(std::uint64_t(image.x)+image.width-1,x+settings_.radius);
        const auto bottom=std::min(std::uint64_t(image.y)+image.height-1,y+settings_.radius);
        const auto count=(right-std::uint64_t(left)+1)*(bottom-std::uint64_t(top)+1);
        const auto source=(std::size_t(y-needed.y)*needed.width+std::size_t(x-needed.x))*3;
        const auto target=(std::size_t(row)*r.width+col)*3;
        const double centers[3]{input.rgb[source],input.rgb[source+1],input.rgb[source+2]};
        double sums[3]{};
        for (auto yy=std::uint64_t(top);yy<=bottom;++yy) {
            auto index=(std::size_t(yy-needed.y)*needed.width+std::size_t(std::uint64_t(left)-needed.x))*3;
            for (auto xx=std::uint64_t(left);xx<=right;++xx,index+=3) for (unsigned c=0;c<3;++c) {
                const double difference=double(input.rgb[index+c])-centers[c];
                sums[c]=sums[c]+difference;
                if (!std::isfinite(difference) || !std::isfinite(sums[c])) throw std::invalid_argument("sharpen nonfinite difference sum");
            }
        }
        for (unsigned c=0;c<3;++c) {
            const double center=centers[c],sum=sums[c];
            const double detail=-(sum/static_cast<double>(count));
            if (!std::isfinite(detail)) throw std::invalid_argument("sharpen nonfinite detail");
            if (detail==0) { result.rgb[target+c]=input.rgb[source+c];continue; }
            const double offset=settings_.amount*detail;
            if (offset==0) { result.rgb[target+c]=input.rgb[source+c];continue; }
            const double mapped=center+offset;
            if (!std::isfinite(offset) || !std::isfinite(mapped) || std::abs(mapped)>std::numeric_limits<float>::max())
                throw std::invalid_argument("sharpen output exceeds finite float32 range");
            result.rgb[target+c]=static_cast<float>(mapped);
        }
    }
    return result;
}

} // namespace rawengine
