#include "MaskOps.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {

bool same(Rect a, Rect b) {
    return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
}

void valid_extent(Rect bounds) {
    constexpr auto maximum=std::numeric_limits<std::uint32_t>::max();
    if (!bounds.width || !bounds.height || bounds.x>maximum-bounds.width || bounds.y>maximum-bounds.height)
        throw std::invalid_argument("mask extent is empty or overflows");
}

bool admitted_level(RenderLevel level) {
    return (level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
           ((level.mip==1 || level.mip==2) && level.quality==RenderQuality::Preview);
}

Rect level_extent(Rect native, RenderLevel level) {
    if (!admitted_level(level)) throw std::invalid_argument("unsupported mask level");
    if (!level.mip) return native;
    const auto scale=1u<<level.mip;
    return {0,0,native.width/scale+(native.width%scale!=0),native.height/scale+(native.height%scale!=0)};
}

void contained(Rect extent, Rect roi) {
    if (!roi.width || !roi.height || roi.x<extent.x || roi.y<extent.y ||
        roi.width>extent.width || roi.height>extent.height ||
        roi.x-extent.x>extent.width-roi.width || roi.y-extent.y>extent.height-roi.height)
        throw std::invalid_argument("mask ROI is outside output extent");
}

void valid_coverage(float value) {
    const auto bits=std::bit_cast<std::uint32_t>(value),magnitude=bits&0x7fffffffu;
    if (magnitude>0x3f800000u || (magnitude && (bits&0x80000000u)))
        throw std::invalid_argument("mask coverage must be finite and in [0,1]");
}

void valid_mode(MaskCombineMode mode) {
    if (mode!=MaskCombineMode::Add && mode!=MaskCombineMode::Subtract && mode!=MaskCombineMode::Intersect)
        throw std::invalid_argument("unsupported mask combination mode");
}

void valid_amount(double amount) {
    if (!std::isfinite(amount) || amount<0 || amount>1)
        throw std::invalid_argument("masked mix amount must be finite and in [0,1]");
}

void actual_mask(const CoverageTile& tile, Rect roi) {
    if (!same(tile.bounds,roi)) throw std::domain_error("coverage node returned wrong ROI");
    validate_coverage_tile(tile);
}

bool working_rgb(ImageDescriptor descriptor) {
    return descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) ||
           descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
}

void actual_rgb(const Tile& tile, Rect roi, ImageDescriptor descriptor) {
    const auto pixels=static_cast<std::uint64_t>(roi.width)*roi.height;
    if (!same(tile.bounds,roi) || tile.descriptor!=descriptor ||
        pixels>std::numeric_limits<std::size_t>::max()/3 || tile.rgb.size()!=pixels*3)
        throw std::domain_error("masked RGB node returned invalid storage,ROI or descriptor");
    for (float value:tile.rgb)
        if (!std::isfinite(value)) throw std::invalid_argument("masked mix RGB input contains nonfinite values");
}

float coverage_store(double value) {
    return value==0 ? 0.0f : static_cast<float>(value);
}

} // namespace

float invert_coverage(float value) {
    valid_coverage(value);
    return coverage_store(1.0-static_cast<double>(value));
}

float combine_coverage(float base, float layer, MaskCombineMode mode) {
    valid_coverage(base);valid_coverage(layer);valid_mode(mode);
    const double a=base,b=layer;
    if (mode==MaskCombineMode::Add) return coverage_store(std::min(1.0,a+b));
    if (mode==MaskCombineMode::Subtract) return coverage_store(std::max(0.0,a-b));
    return coverage_store(a*b);
}

Tile masked_mix_rgb(const Tile& base, const Tile& layer, const CoverageTile& mask, double amount) {
    valid_amount(amount);validate_coverage_tile(mask);
    if (!working_rgb(base.descriptor) || layer.descriptor!=base.descriptor)
        throw std::invalid_argument("masked mix requires matching exact working RGB descriptors");
    actual_rgb(base,mask.bounds,base.descriptor);actual_rgb(layer,mask.bounds,base.descriptor);
    Tile output{mask.bounds,std::vector<float>(base.rgb.size()),base.descriptor};
    for (std::size_t i=0;i<mask.coverage.size();++i) {
        const double weight=static_cast<double>(mask.coverage[i])*amount;
        for (std::size_t c=0;c<3;++c) {
            const auto p=i*3+c;
            if (weight==0 || std::bit_cast<std::uint32_t>(base.rgb[p])==std::bit_cast<std::uint32_t>(layer.rgb[p]))
                output.rgb[p]=base.rgb[p];
            else if (weight==1) output.rgb[p]=layer.rgb[p];
            else {
                const double delta=static_cast<double>(layer.rgb[p])-static_cast<double>(base.rgb[p]);
                const double scaled=weight*delta;
                const double mapped=static_cast<double>(base.rgb[p])+scaled;
                constexpr double maximum=std::numeric_limits<float>::max();
                if (!std::isfinite(mapped) || mapped < -maximum || mapped > maximum)
                    throw std::overflow_error("masked mix result exceeds finite float32 range");
                output.rgb[p]=static_cast<float>(mapped);
            }
        }
    }
    return output;
}

Rect CoverageNode::output_bounds(RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("coverage node does not support level");
    return level_extent(native_bounds(),level);
}

CoverageRasterNode::CoverageRasterNode(CoverageImage image):source_(std::move(image)) {}
CoverageTile CoverageRasterNode::render_level(Rect bounds,RenderLevel level) const {return source_.render_level(bounds,level);}
bool CoverageRasterNode::supports_level(RenderLevel level) const noexcept {return source_.supports_level(level);}
Rect CoverageRasterNode::native_bounds() const noexcept {return source_.source_bounds();}
std::optional<std::array<std::uint8_t,32>> CoverageRasterNode::source_fingerprint() const {return source_.source_fingerprint();}
Rect CoverageRasterNode::required_native_region(Rect bounds,RenderLevel level) const {return source_.required_native_region(bounds,level);}

CoverageInvertNode::CoverageInvertNode(std::shared_ptr<const CoverageNode> input):input_(std::move(input)) {
    if (!input_) throw std::invalid_argument("invert mask input is null");
    valid_extent(input_->native_bounds());
}
bool CoverageInvertNode::supports_level(RenderLevel level) const noexcept {return admitted_level(level)&&input_->supports_level(level);}
Rect CoverageInvertNode::native_bounds() const noexcept {return input_->native_bounds();}
CoverageTile CoverageInvertNode::render_level(Rect bounds,RenderLevel level) const {
    contained(output_bounds(level),bounds);
    auto tile=input_->render_level(bounds,level);actual_mask(tile,bounds);
    for (auto& value:tile.coverage) value=invert_coverage(value);
    return tile;
}

CoverageCombineNode::CoverageCombineNode(std::shared_ptr<const CoverageNode> base,
    std::shared_ptr<const CoverageNode> layer,MaskCombineMode mode)
    :base_(std::move(base)),layer_(std::move(layer)),mode_(mode) {
    valid_mode(mode_);
    if (!base_ || !layer_ || !same(base_->native_bounds(),layer_->native_bounds()))
        throw std::invalid_argument("mask combination requires matching nonnull extents");
    valid_extent(base_->native_bounds());
}
bool CoverageCombineNode::supports_level(RenderLevel level) const noexcept {
    return admitted_level(level)&&base_->supports_level(level)&&layer_->supports_level(level);
}
Rect CoverageCombineNode::native_bounds() const noexcept {return base_->native_bounds();}
CoverageTile CoverageCombineNode::render_level(Rect bounds,RenderLevel level) const {
    contained(output_bounds(level),bounds);
    auto base=base_->render_level(bounds,level);actual_mask(base,bounds);
    const auto layer=layer_->render_level(bounds,level);actual_mask(layer,bounds);
    for (std::size_t i=0;i<base.coverage.size();++i)
        base.coverage[i]=combine_coverage(base.coverage[i],layer.coverage[i],mode_);
    return base;
}

MaskedMixNode::MaskedMixNode(std::shared_ptr<const Node> base,std::shared_ptr<const Node> layer,
    std::shared_ptr<const CoverageNode> mask,Rect bounds,double amount)
    :base_(std::move(base)),layer_(std::move(layer)),mask_(std::move(mask)),bounds_(bounds),amount_(amount) {
    valid_extent(bounds_);valid_amount(amount_);
    if (!base_ || !layer_ || !mask_ || !same(mask_->native_bounds(),bounds_) ||
        !working_rgb(base_->output_descriptor()) || layer_->output_descriptor()!=base_->output_descriptor())
        throw std::invalid_argument("masked mix requires matching RGB domain and coverage extent");
}
Tile MaskedMixNode::render(Rect bounds) const {return render_level(bounds,{});}
bool MaskedMixNode::supports_level(RenderLevel level) const noexcept {
    return admitted_level(level)&&base_->supports_level(level)&&layer_->supports_level(level)&&mask_->supports_level(level);
}
ImageDescriptor MaskedMixNode::output_descriptor() const noexcept {return base_->output_descriptor();}
Rect MaskedMixNode::input_region_level(Rect output,Rect input_bounds,RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("masked mix has no mapping for this level");
    contained(input_bounds,output);return output;
}
Tile MaskedMixNode::render_level(Rect bounds,RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("masked mix does not support level");
    contained(level_extent(bounds_,level),bounds);
    const auto base=base_->render_level(bounds,level),layer=layer_->render_level(bounds,level);
    const auto mask=mask_->render_level(bounds,level);
    actual_mask(mask,bounds);
    return masked_mix_rgb(base,layer,mask,amount_);
}

} // namespace rawengine
