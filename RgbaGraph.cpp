#include "RgbaGraph.hpp"
#include "Sha256.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <mutex>

namespace rawengine {
namespace {

bool same(Rect a, Rect b) {
    return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
}
void valid_extent(Rect bounds) {
    constexpr auto max=std::numeric_limits<std::uint32_t>::max();
    if (!bounds.width || !bounds.height || bounds.x>max-bounds.width || bounds.y>max-bounds.height)
        throw std::invalid_argument("RGBA extent is empty or overflows");
}
void valid_space(WorkingSpace space) {
    if (space!=WorkingSpace::LinearProPhotoD50 && space!=WorkingSpace::LinearRec2020D65)
        throw std::invalid_argument("RGBA requires an admitted scene-linear working space");
}
bool admitted(RenderLevel level) {
    return (level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
           ((level.mip==1 || level.mip==2) && level.quality==RenderQuality::Preview);
}
Rect extent(Rect native, RenderLevel level) {
    if (!admitted(level)) throw std::invalid_argument("unsupported RGBA level");
    if (!level.mip) return native;
    const auto scale=1u<<level.mip;
    return {0,0,native.width/scale+(native.width%scale!=0),native.height/scale+(native.height%scale!=0)};
}
void contained(Rect bounds, Rect roi) {
    if (!roi.width || !roi.height || roi.x<bounds.x || roi.y<bounds.y ||
        roi.width>bounds.width || roi.height>bounds.height ||
        roi.x-bounds.x>bounds.width-roi.width || roi.y-bounds.y>bounds.height-roi.height)
        throw std::invalid_argument("RGBA ROI is outside extent");
}
std::size_t sample_count(std::uint64_t width, std::uint64_t height, std::uint64_t channels=1) {
    const auto max=std::numeric_limits<std::size_t>::max()/sizeof(float);
    if (width>max/channels || height>max/(width*channels))
        throw std::invalid_argument("RGBA sample size overflows");
    const auto count=width*height*channels;
    if (count>std::vector<float>{}.max_size())
        throw std::invalid_argument("RGBA vector size exceeds capacity");
    return static_cast<std::size_t>(count);
}
Rect native_roi(Rect native, Rect output, RenderLevel level) {
    contained(extent(native,level),output);
    if (!level.mip) return output;
    const auto scale=1u<<level.mip;
    const auto x=static_cast<std::uint64_t>(output.x)*scale;
    const auto y=static_cast<std::uint64_t>(output.y)*scale;
    const auto end_x=std::min<std::uint64_t>((static_cast<std::uint64_t>(output.x)+output.width)*scale,native.width);
    const auto end_y=std::min<std::uint64_t>((static_cast<std::uint64_t>(output.y)+output.height)*scale,native.height);
    return {static_cast<std::uint32_t>(native.x+x),static_cast<std::uint32_t>(native.y+y),
            static_cast<std::uint32_t>(end_x-x),static_cast<std::uint32_t>(end_y-y)};
}
void resources(Rect input, Rect output, std::size_t native_channels, std::size_t output_channels) {
    const auto n=sample_count(input.width,input.height,native_channels);
    const auto p=sample_count(output.width,output.height,output_channels);
    if (n>std::numeric_limits<std::size_t>::max()/sizeof(float)-p)
        throw std::invalid_argument("RGBA combined payload size overflows");
}
void actual(const PremultipliedRgbaTile& tile, Rect roi, WorkingSpace space) {
    if (!same(tile.bounds,roi) || tile.working_space!=space)
        throw std::domain_error("RGBA node returned wrong ROI or working space");
    validate_premultiplied_rgba_tile(tile);
}
PremultipliedRgbaPixel load(const std::vector<float>& data, std::size_t offset) {
    return {{data[offset],data[offset+1],data[offset+2]},data[offset+3]};
}
// Input rectangle consists of complete native cells for this output rectangle.
std::vector<float> reduce(const std::vector<float>& data, Rect input, Rect output,
                          RenderLevel level, std::size_t channels, bool rgba) {
    std::vector<float> result(sample_count(output.width,output.height,channels));
    const auto scale=1u<<level.mip;
    for (std::uint32_t y=0;y<output.height;++y) {
        const auto y0=static_cast<std::uint64_t>(y)*scale;
        const auto y1=std::min<std::uint64_t>(y0+scale,input.height);
        for (std::uint32_t x=0;x<output.width;++x) {
            const auto x0=static_cast<std::uint64_t>(x)*scale;
            const auto x1=std::min<std::uint64_t>(x0+scale,input.width);
            const auto offset=(static_cast<std::size_t>(y)*output.width+x)*channels;
            for (std::size_t c=0;c<channels;++c) {
                double sum=0;
                for (auto v=y0;v<y1;++v)
                    for (auto u=x0;u<x1;++u)
                        sum+=static_cast<double>(data[(static_cast<std::size_t>(v)*input.width+u)*channels+c]);
                result[offset+c]=static_cast<float>(sum/static_cast<double>((x1-x0)*(y1-y0)));
            }
            if (rgba && result[offset+3]==0)
                for (std::size_t c=0;c<4;++c) result[offset+c]=0;
        }
    }
    return result;
}
PremultipliedRgbaTile finish(PremultipliedRgbaTile tile, Rect output, RenderLevel level) {
    if (!level.mip) return tile;
    auto values=reduce(tile.rgba,tile.bounds,output,level,4,true);
    return {output,std::move(values),tile.working_space};
}
void hash_u32(Sha256& hash,std::uint32_t value) {
    const std::uint8_t bytes[]{static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8),
        static_cast<std::uint8_t>(value>>16),static_cast<std::uint8_t>(value>>24)};
    hash.update(bytes,sizeof(bytes));
}
RenderLevel native_level(bool supported) {
    if (!supported) throw std::invalid_argument("RGBA input level is unsupported");
    return {};
}
Rect mapping(Rect bounds, Rect output, Rect input, RenderLevel level, bool supported) {
    (void)native_level(supported);
    if (!same(input,bounds)) throw std::invalid_argument("RGBA input extent differs");
    return native_roi(bounds,output,level);
}

} // namespace

struct RgbaImage::State {
    RgbaMetadata metadata;
    std::vector<float> samples;
    mutable std::once_flag once;
    mutable std::array<std::uint8_t,32> fingerprint{};
};

RgbaImage::RgbaImage(RgbaMetadata metadata,const std::vector<float>& input) {
    valid_extent(metadata.bounds);valid_space(metadata.working_space);
    const auto width=static_cast<std::uint64_t>(metadata.bounds.width)*4;
    const auto stride=metadata.row_stride_samples ? metadata.row_stride_samples : width;
    if (stride<width || input.size()!=sample_count(stride,metadata.bounds.height))
        throw std::invalid_argument("RGBA source samples differ from stride and height");
    for (std::uint32_t y=0;y<metadata.bounds.height;++y)
        for (std::uint32_t x=0;x<metadata.bounds.width;++x)
            validate_premultiplied_rgba_pixel(load(input,static_cast<std::size_t>(y*stride+x*4ull)));
    auto state=std::make_shared<State>();
    state->metadata={metadata.bounds,width,metadata.working_space};
    state->samples.resize(sample_count(metadata.bounds.width,metadata.bounds.height,4));
    for (std::uint32_t y=0;y<metadata.bounds.height;++y)
        for (std::uint32_t x=0;x<metadata.bounds.width;++x) {
            const auto from=static_cast<std::size_t>(y*stride+x*4ull);
            const auto to=(static_cast<std::size_t>(y)*metadata.bounds.width+x)*4;
            for (std::size_t c=0;c<4;++c)
                state->samples[to+c]=input[from+3]==0 ? 0.0f : input[from+c];
        }
    state_=std::move(state);
}
const RgbaMetadata& RgbaImage::metadata() const noexcept { return state_->metadata; }
const std::vector<float>& RgbaImage::samples() const noexcept { return state_->samples; }
std::array<std::uint8_t,32> RgbaImage::fingerprint() const {
    std::call_once(state_->once,[&] {
        Sha256 hash;
        constexpr char tag[]="librawops.source.rgba.premult.f32.v1";
        hash.update(tag,sizeof(tag));
        const auto b=state_->metadata.bounds;
        hash_u32(hash,b.x);hash_u32(hash,b.y);hash_u32(hash,b.width);hash_u32(hash,b.height);
        hash_u32(hash,state_->metadata.working_space==WorkingSpace::LinearProPhotoD50 ? 1u : 2u);
        for (float value:state_->samples) hash_u32(hash,std::bit_cast<std::uint32_t>(value));
        state_->fingerprint=hash.finish();
    });
    return state_->fingerprint;
}

StraightRgbFloat64Tile straight_rgb_float64(const PremultipliedRgbaTile& tile) {
    validate_premultiplied_rgba_tile(tile);
    const auto count=sample_count(tile.bounds.width,tile.bounds.height,3);
    if (count>std::numeric_limits<std::size_t>::max()/sizeof(double) || count>std::vector<double>{}.max_size())
        throw std::invalid_argument("wide straight RGB storage size overflows");
    StraightRgbFloat64Tile output{tile.bounds,std::vector<double>(count),tile.working_space};
    for (std::size_t i=0;i<count/3;++i) {
        const auto values=straight_rgb(load(tile.rgba,i*4));
        for (std::size_t c=0;c<3;++c) output.rgb[i*3+c]=values[c];
    }
    return output;
}

Rect RgbaNode::output_bounds(RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("RGBA node level unsupported");
    return extent(native_bounds(),level);
}
Rect RgbaNode::input_region_level(Rect output,Rect input,RenderLevel level) const {
    return mapping(native_bounds(),output,input,level,supports_level(level));
}
RenderLevel RgbaNode::input_level(RenderLevel level) const { return native_level(supports_level(level)); }

RgbaRasterNode::RgbaRasterNode(RgbaImage image):image_(std::move(image)) {}
bool RgbaRasterNode::supports_level(RenderLevel level) const noexcept { return admitted(level); }
Rect RgbaRasterNode::native_bounds() const noexcept { return image_.metadata().bounds; }
WorkingSpace RgbaRasterNode::working_space() const noexcept { return image_.metadata().working_space; }
std::optional<std::array<std::uint8_t,32>> RgbaRasterNode::source_fingerprint() const { return image_.fingerprint(); }
PremultipliedRgbaTile RgbaRasterNode::render_level(Rect bounds,RenderLevel level) const {
    const auto input=input_region_level(bounds,native_bounds(),level);
    resources(input,bounds,4,4);
    PremultipliedRgbaTile tile{input,std::vector<float>(sample_count(input.width,input.height,4)),working_space()};
    const auto native=native_bounds();
    for (std::uint32_t y=0;y<input.height;++y) {
        const auto offset=(static_cast<std::size_t>(input.y-native.y+y)*native.width+input.x-native.x)*4;
        std::copy_n(image_.samples().begin()+offset,static_cast<std::size_t>(input.width)*4,
                    tile.rgba.begin()+static_cast<std::size_t>(y)*input.width*4);
    }
    return finish(std::move(tile),bounds,level);
}

RgbPremultiplyNode::RgbPremultiplyNode(std::shared_ptr<const Node> image,
    std::shared_ptr<const CoverageNode> alpha,Rect bounds)
    :image_(std::move(image)),alpha_(std::move(alpha)),bounds_(bounds) {
    valid_extent(bounds_);
    if (!image_ || !alpha_ || !same(alpha_->native_bounds(),bounds_))
        throw std::invalid_argument("premultiply requires nonnull matching native inputs");
    const auto descriptor=image_->output_descriptor();
    if (descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50)) space_=WorkingSpace::LinearProPhotoD50;
    else if (descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)) space_=WorkingSpace::LinearRec2020D65;
    else throw std::invalid_argument("premultiply requires exact working RGB descriptor");
}
bool RgbPremultiplyNode::supports_level(RenderLevel level) const noexcept {
    return admitted(level)&&image_->supports_level({})&&alpha_->supports_level({});
}
PremultipliedRgbaTile RgbPremultiplyNode::render_level(Rect bounds,RenderLevel level) const {
    const auto input=input_region_level(bounds,bounds_,level);
    resources(input,bounds,8,4);
    const auto image=image_->render_level(input,{});
    const auto alpha=alpha_->render_level(input,{});
    if (!same(image.bounds,input) || !same(alpha.bounds,input) || image.descriptor!=ImageDescriptor::scene_linear(space_))
        throw std::domain_error("premultiply actual input ROI or descriptor differs");
    return finish(premultiply_rgb(image,alpha),bounds,level);
}

RgbaApplyCoverageNode::RgbaApplyCoverageNode(std::shared_ptr<const RgbaNode> image,
    std::shared_ptr<const CoverageNode> coverage):image_(std::move(image)),coverage_(std::move(coverage)) {
    if (!image_ || !coverage_) throw std::invalid_argument("RGBA coverage inputs are null");
    bounds_=image_->native_bounds();space_=image_->working_space();valid_extent(bounds_);valid_space(space_);
    if (!same(bounds_,coverage_->native_bounds())) throw std::invalid_argument("RGBA coverage extents differ");
}
bool RgbaApplyCoverageNode::supports_level(RenderLevel level) const noexcept {
    return admitted(level)&&image_->supports_level({})&&coverage_->supports_level({});
}
PremultipliedRgbaTile RgbaApplyCoverageNode::render_level(Rect bounds,RenderLevel level) const {
    const auto input=input_region_level(bounds,bounds_,level);resources(input,bounds,9,4);
    const auto image=image_->render_level(input,{});actual(image,input,space_);
    const auto mask=coverage_->render_level(input,{});
    if (!same(mask.bounds,input)) throw std::domain_error("RGBA coverage returned wrong ROI");
    return finish(apply_coverage(image,mask),bounds,level);
}

RgbaSourceOverNode::RgbaSourceOverNode(std::shared_ptr<const RgbaNode> source,
    std::shared_ptr<const RgbaNode> backdrop):source_(std::move(source)),backdrop_(std::move(backdrop)) {
    if (!source_ || !backdrop_) throw std::invalid_argument("source-over inputs are null");
    bounds_=source_->native_bounds();space_=source_->working_space();valid_extent(bounds_);valid_space(space_);
    if (!same(bounds_,backdrop_->native_bounds()) || space_!=backdrop_->working_space())
        throw std::invalid_argument("source-over native bounds or spaces differ");
}
bool RgbaSourceOverNode::supports_level(RenderLevel level) const noexcept {
    return admitted(level)&&source_->supports_level({})&&backdrop_->supports_level({});
}
PremultipliedRgbaTile RgbaSourceOverNode::render_level(Rect bounds,RenderLevel level) const {
    const auto input=input_region_level(bounds,bounds_,level);resources(input,bounds,12,4);
    const auto source=source_->render_level(input,{});actual(source,input,space_);
    const auto backdrop=backdrop_->render_level(input,{});actual(backdrop,input,space_);
    return finish(source_over(source,backdrop),bounds,level);
}

RgbaAlphaNode::RgbaAlphaNode(std::shared_ptr<const RgbaNode> image):image_(std::move(image)) {
    if (!image_) throw std::invalid_argument("alpha input is null");
    bounds_=image_->native_bounds();space_=image_->working_space();valid_extent(bounds_);valid_space(space_);
}
bool RgbaAlphaNode::supports_level(RenderLevel level) const noexcept { return admitted(level)&&image_->supports_level({}); }
RenderLevel RgbaAlphaNode::input_level(RenderLevel level) const { return native_level(supports_level(level)); }
Rect RgbaAlphaNode::input_region_level(Rect output,Rect input,RenderLevel level) const {
    return mapping(bounds_,output,input,level,supports_level(level));
}
CoverageTile RgbaAlphaNode::render_level(Rect bounds,RenderLevel level) const {
    const auto input=input_region_level(bounds,bounds_,level);resources(input,bounds,5,1);
    const auto image=image_->render_level(input,{});actual(image,input,space_);
    std::vector<float> values(sample_count(input.width,input.height));
    for (std::size_t i=0;i<values.size();++i) values[i]=image.rgba[i*4+3]==0 ? 0.0f : image.rgba[i*4+3];
    if (level.mip) values=reduce(values,input,bounds,level,1,false);
    return {bounds,std::move(values)};
}

RgbaStraightRgbNode::RgbaStraightRgbNode(std::shared_ptr<const RgbaNode> image):image_(std::move(image)) {
    if (!image_) throw std::invalid_argument("straight RGB input is null");
    bounds_=image_->native_bounds();space_=image_->working_space();valid_extent(bounds_);valid_space(space_);
}
bool RgbaStraightRgbNode::supports_level(RenderLevel level) const noexcept { return admitted(level)&&image_->supports_level({}); }
RenderLevel RgbaStraightRgbNode::input_level(RenderLevel level) const { return native_level(supports_level(level)); }
Rect RgbaStraightRgbNode::output_bounds(RenderLevel level) const {
    (void)input_level(level);return extent(bounds_,level);
}
Rect RgbaStraightRgbNode::input_region_level(Rect output,Rect input,RenderLevel level) const {
    return mapping(bounds_,output,input,level,supports_level(level));
}
Tile RgbaStraightRgbNode::render_level(Rect bounds,RenderLevel level) const {
    const auto input=input_region_level(bounds,bounds_,level);resources(input,bounds,7,3);
    const auto image=image_->render_level(input,{});actual(image,input,space_);
    std::vector<float> values(sample_count(input.width,input.height,3));
    for (std::size_t i=0;i<values.size()/3;++i) {
        const auto rgb=straight_rgb_float32(load(image.rgba,i*4));
        for (std::size_t c=0;c<3;++c) values[i*3+c]=rgb[c];
    }
    if (level.mip) values=reduce(values,input,bounds,level,3,false);
    return {bounds,std::move(values),output_descriptor()};
}

} // namespace rawengine
