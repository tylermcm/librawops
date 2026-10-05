#include "RangeMask.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}
void extent(Rect bounds) {
    constexpr auto maximum=std::numeric_limits<std::uint32_t>::max();
    if (!bounds.width || !bounds.height || bounds.x>maximum-bounds.width || bounds.y>maximum-bounds.height)
        throw std::invalid_argument("range mask extent is empty or overflows");
}
void scalar_settings(const ScalarRangeSettings& settings,double low,double high) {
    for (std::size_t i=0;i<4;++i) {
        const auto value=settings.edges[i];
        if (!std::isfinite(value) || value<low || value>high || (i && value<settings.edges[i-1]))
            throw std::invalid_argument("range mask edges must be finite, ordered and inside the selected domain");
    }
}
std::array<double,2> weights(ImageDescriptor descriptor) {
    if (descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50))
        return {0.28807112822929337,0.00008565396060525903};
    if (descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        return {0.26270021201126703,0.059301716469861945};
    throw std::invalid_argument("range guide requires scene-linear ProPhoto/D50 or Rec.2020/D65 RGB");
}
void rgb_value(std::array<float,3> rgb) {
    for (auto value:rgb) if (!std::isfinite(value)) throw std::domain_error("range guide RGB is nonfinite");
}
float stored(double value,bool invert) {
    if (!std::isfinite(value) || value<0 || value>1) throw std::domain_error("range selection is outside coverage domain");
    const auto result=value==0?0.0f:static_cast<float>(value);
    return invert?invert_coverage(result):result;
}
double scalar(double t,const ScalarRangeSettings& settings) {
    if (!std::isfinite(t)) throw std::domain_error("range selector is nonfinite");
    const auto& e=settings.edges;
    if (t>=e[1] && t<=e[2]) return 1;
    if (t<=e[0] || t>=e[3]) return 0;
    const double numerator=t<e[1]?t-e[0]:e[3]-t;
    const double denominator=t<e[1]?e[1]-e[0]:e[3]-e[2];
    const double value=numerator/denominator;
    if (!std::isfinite(value)) throw std::domain_error("range transition is nonfinite");
    return std::clamp(value,0.0,1.0);
}
float luminance(std::array<float,3> rgb,ImageDescriptor descriptor,const ScalarRangeSettings& settings) {
    const auto w=weights(descriptor);
    const double r=rgb[0],g=rgb[1],b=rgb[2],dr=r-g,db=b-g;
    const double pr=w[0]*dr,pb=w[1]*db,partial=g+pr,y=partial+pb;
    if (!std::isfinite(dr) || !std::isfinite(db) || !std::isfinite(pr) || !std::isfinite(pb) ||
        !std::isfinite(partial) || !std::isfinite(y)) throw std::domain_error("luminance selector stage is nonfinite");
    return stored(scalar(y,settings),settings.invert);
}
float color(std::array<float,3> rgb,const ColorRangeSettings& settings) {
    std::array<double,3> squared;
    for (std::size_t c=0;c<3;++c) {
        const double diff=static_cast<double>(rgb[c])-settings.center[c],q=diff/settings.scales[c],sq=q*q;
        if (!std::isfinite(diff) || !std::isfinite(q) || !std::isfinite(sq))
            throw std::domain_error("color selector stage is nonfinite");
        squared[c]=sq;
    }
    const double s01=squared[0]+squared[1],distance2=s01+squared[2];
    const double inner2=settings.inner*settings.inner,outer2=settings.outer*settings.outer;
    if (!std::isfinite(s01) || !std::isfinite(distance2) || !std::isfinite(inner2) || !std::isfinite(outer2))
        throw std::domain_error("color selector distance is nonfinite");
    double value;
    if (distance2<=inner2) value=1;
    else if (distance2>=outer2) value=0;
    else {
        const double numerator=outer2-distance2,denominator=outer2-inner2,ratio=numerator/denominator;
        if (!std::isfinite(ratio)) throw std::domain_error("color selector transition is nonfinite");
        value=std::clamp(ratio,0.0,1.0);
    }
    return stored(value,settings.invert);
}
bool admitted(RenderLevel level) {
    return (level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
           ((level.mip==1 || level.mip==2) && level.quality==RenderQuality::Preview);
}
RenderLevel native_level(const CoverageNode& node,RenderLevel level) {
    if (!node.supports_level(level)) throw std::invalid_argument("unsupported range mask level");
    return {0,RenderQuality::Final};
}
Rect native_region(const CoverageNode& node,Rect output,Rect input,RenderLevel level) {
    if (!same(input,node.native_bounds())) throw std::invalid_argument("range mask input extent mismatch");
    const auto bounds=node.output_bounds(level);
    if (!output.width || !output.height || output.x<bounds.x || output.y<bounds.y ||
        output.width>bounds.width || output.height>bounds.height || output.x-bounds.x>bounds.width-output.width ||
        output.y-bounds.y>bounds.height-output.height) throw std::out_of_range("range mask ROI is outside output extent");
    if (!level.mip) return output;
    const auto native=node.native_bounds();const std::uint64_t scale=1u<<level.mip;
    const auto x=static_cast<std::uint64_t>(output.x)*scale,y=static_cast<std::uint64_t>(output.y)*scale;
    const auto right=std::min((static_cast<std::uint64_t>(output.x)+output.width)*scale,static_cast<std::uint64_t>(native.width));
    const auto bottom=std::min((static_cast<std::uint64_t>(output.y)+output.height)*scale,static_cast<std::uint64_t>(native.height));
    return {static_cast<std::uint32_t>(native.x+x),static_cast<std::uint32_t>(native.y+y),
            static_cast<std::uint32_t>(right-x),static_cast<std::uint32_t>(bottom-y)};
}
std::size_t count(Rect bounds,std::size_t channels=1) {
    const auto pixels=static_cast<std::uint64_t>(bounds.width)*bounds.height;
    if (pixels>std::numeric_limits<std::size_t>::max()/sizeof(float)/channels ||
        pixels>std::vector<float>{}.max_size()/channels) throw std::invalid_argument("range mask storage capacity overflows");
    return static_cast<std::size_t>(pixels);
}
void rgb_tile(const Tile& tile,Rect request,ImageDescriptor descriptor) {
    if (!same(tile.bounds,request) || tile.descriptor!=descriptor || tile.rgb.size()!=count(request,3)*3)
        throw std::domain_error("range RGB guide returned wrong ROI, descriptor or storage");
    for (float value:tile.rgb) if (!std::isfinite(value)) throw std::domain_error("range RGB guide contains nonfinite samples");
}
void coverage_tile(const CoverageTile& tile,Rect request) {
    if (!same(tile.bounds,request)) throw std::domain_error("range scalar input returned wrong ROI");
    validate_coverage_tile(tile);
}
template<class Evaluate> CoverageTile mapped(CoverageTile native,
                                           Rect bounds,RenderLevel level,Evaluate evaluate) {
    for (std::size_t i=0;i<native.coverage.size();++i) {
        const float selection=evaluate(i);
        if (selection==0) native.coverage[i]=0;
        else if (selection!=1) {
            const double product=static_cast<double>(native.coverage[i])*static_cast<double>(selection);
            native.coverage[i]=stored(product,false);
        }
    }
    if (!level.mip) return native;
    CoverageTile output{bounds,std::vector<float>(count(bounds))};const auto scale=1u<<level.mip;
    for (std::uint32_t y=0;y<bounds.height;++y) for (std::uint32_t x=0;x<bounds.width;++x) {
        const auto sx=static_cast<std::uint64_t>(x)*scale,sy=static_cast<std::uint64_t>(y)*scale;
        const auto ex=std::min(sx+scale,static_cast<std::uint64_t>(native.bounds.width));
        const auto ey=std::min(sy+scale,static_cast<std::uint64_t>(native.bounds.height));
        double sum=0;
        for (auto v=sy;v<ey;++v) for (auto u=sx;u<ex;++u)
            sum+=static_cast<double>(native.coverage[static_cast<std::size_t>(v)*native.bounds.width+u]);
        const double samples=static_cast<double>((ex-sx)*(ey-sy));
        output.coverage[static_cast<std::size_t>(y)*bounds.width+x]=stored(sum/samples,false);
    }
    return output;
}
void rgb_inputs(const std::shared_ptr<const CoverageNode>& mask,const std::shared_ptr<const Node>& image,Rect image_bounds) {
    if (!mask || !image) throw std::invalid_argument("range mask or RGB guide is null");
    extent(mask->native_bounds());extent(image_bounds);
    if (!same(mask->native_bounds(),image_bounds)) throw std::invalid_argument("range mask and RGB guide extents differ");
    (void)weights(image->output_descriptor());
}
template<class Evaluate> CoverageTile rgb_render(const CoverageNode& node,const CoverageNode& mask,const Node& image,
                                                Rect bounds,RenderLevel level,Evaluate evaluate) {
    const auto request=native_region(node,bounds,mask.native_bounds(),level);
    (void)count(request,3);(void)count(bounds);
    auto native=mask.render_level(request,native_level(node,level));coverage_tile(native,request);
    auto guide=image.render_level(request,{});rgb_tile(guide,request,image.output_descriptor());
    return mapped(std::move(native),bounds,level,[&](std::size_t i) {
        return evaluate(std::array<float,3>{guide.rgb[3*i],guide.rgb[3*i+1],guide.rgb[3*i+2]});
    });
}
} // namespace

void validate_luminance_range_settings(const ScalarRangeSettings& settings) {scalar_settings(settings,-65536,65536);}
void validate_depth_range_settings(const ScalarRangeSettings& settings) {scalar_settings(settings,0,1);}
void validate_color_range_settings(const ColorRangeSettings& settings) {
    for (auto v:settings.center) if (!std::isfinite(v) || std::abs(v)>65536) throw std::invalid_argument("color range center exceeds finite bounds");
    for (auto v:settings.scales) if (!std::isfinite(v) || v<0x1p-8 || v>65536) throw std::invalid_argument("color range scale exceeds finite bounds");
    if (!std::isfinite(settings.inner) || !std::isfinite(settings.outer) || settings.outer<0x1p-8 || settings.outer>65536 ||
        settings.inner<0 || settings.inner>settings.outer) throw std::invalid_argument("color range radii exceed finite ordered bounds");
}
float evaluate_luminance_range_mask(std::array<float,3> rgb,ImageDescriptor descriptor,const ScalarRangeSettings& settings) {
    validate_luminance_range_settings(settings);rgb_value(rgb);return luminance(rgb,descriptor,settings);
}
float evaluate_color_range_mask(std::array<float,3> rgb,ImageDescriptor descriptor,const ColorRangeSettings& settings) {
    validate_color_range_settings(settings);(void)weights(descriptor);rgb_value(rgb);return color(rgb,settings);
}
float evaluate_depth_range_mask(float depth,const ScalarRangeSettings& settings) {
    validate_depth_range_settings(settings);
    if (!std::isfinite(depth) || depth<0 || depth>1) throw std::domain_error("depth guide must be finite in [0,1]");
    return stored(scalar(depth,settings),settings.invert);
}
CoverageLuminanceRangeNode::CoverageLuminanceRangeNode(std::shared_ptr<const CoverageNode> mask,std::shared_ptr<const Node> image,
    Rect image_bounds,const ScalarRangeSettings& settings):mask_(std::move(mask)),image_(std::move(image)),settings_(settings) {
    rgb_inputs(mask_,image_,image_bounds);validate_luminance_range_settings(settings_);
}
bool CoverageLuminanceRangeNode::supports_level(RenderLevel level) const noexcept {return admitted(level)&&mask_->supports_level({})&&image_->supports_level({});}
Rect CoverageLuminanceRangeNode::native_bounds() const noexcept {return mask_->native_bounds();}
RenderLevel CoverageLuminanceRangeNode::input_level(RenderLevel level) const {return native_level(*this,level);}
Rect CoverageLuminanceRangeNode::input_region_level(Rect output,Rect input,RenderLevel level) const {return native_region(*this,output,input,level);}
CoverageTile CoverageLuminanceRangeNode::render_level(Rect bounds,RenderLevel level) const {
    return rgb_render(*this,*mask_,*image_,bounds,level,[&](auto rgb) {return luminance(rgb,image_->output_descriptor(),settings_);});
}
CoverageColorRangeNode::CoverageColorRangeNode(std::shared_ptr<const CoverageNode> mask,std::shared_ptr<const Node> image,
    Rect image_bounds,const ColorRangeSettings& settings):mask_(std::move(mask)),image_(std::move(image)),settings_(settings) {
    rgb_inputs(mask_,image_,image_bounds);validate_color_range_settings(settings_);
}
bool CoverageColorRangeNode::supports_level(RenderLevel level) const noexcept {return admitted(level)&&mask_->supports_level({})&&image_->supports_level({});}
Rect CoverageColorRangeNode::native_bounds() const noexcept {return mask_->native_bounds();}
RenderLevel CoverageColorRangeNode::input_level(RenderLevel level) const {return native_level(*this,level);}
Rect CoverageColorRangeNode::input_region_level(Rect output,Rect input,RenderLevel level) const {return native_region(*this,output,input,level);}
CoverageTile CoverageColorRangeNode::render_level(Rect bounds,RenderLevel level) const {
    return rgb_render(*this,*mask_,*image_,bounds,level,[&](auto rgb) {return color(rgb,settings_);});
}
CoverageDepthRangeNode::CoverageDepthRangeNode(std::shared_ptr<const CoverageNode> mask,std::shared_ptr<const CoverageNode> depth,
    const ScalarRangeSettings& settings):mask_(std::move(mask)),depth_(std::move(depth)),settings_(settings) {
    if (!mask_ || !depth_) throw std::invalid_argument("range mask or depth guide is null");
    extent(mask_->native_bounds());extent(depth_->native_bounds());
    if (!same(mask_->native_bounds(),depth_->native_bounds())) throw std::invalid_argument("range mask and depth guide extents differ");
    validate_depth_range_settings(settings_);
}
bool CoverageDepthRangeNode::supports_level(RenderLevel level) const noexcept {return admitted(level)&&mask_->supports_level({})&&depth_->supports_level({});}
Rect CoverageDepthRangeNode::native_bounds() const noexcept {return mask_->native_bounds();}
RenderLevel CoverageDepthRangeNode::input_level(RenderLevel level) const {return native_level(*this,level);}
Rect CoverageDepthRangeNode::input_region_level(Rect output,Rect input,RenderLevel level) const {return native_region(*this,output,input,level);}
CoverageTile CoverageDepthRangeNode::render_level(Rect bounds,RenderLevel level) const {
    const auto request=native_region(*this,bounds,mask_->native_bounds(),level);(void)count(request);(void)count(bounds);
    auto native=mask_->render_level(request,native_level(*this,level));coverage_tile(native,request);
    auto guide=depth_->render_level(request,{});coverage_tile(guide,request);
    return mapped(std::move(native),bounds,level,[&](std::size_t i) {return stored(scalar(guide.coverage[i],settings_),settings_.invert);});
}
} // namespace rawengine
