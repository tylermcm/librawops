#include "ToneOps.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rawengine {
namespace {
std::array<double,2> working_y_weights(ImageDescriptor descriptor) {
    // Same once-rounded binary64 Y policy pinned by saturation/vibrance v1.
    if (descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50))
        return {0.28807112822929337,0.00008565396060525903};
    if (descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        return {0.26270021201126703,0.059301716469861945};
    throw std::invalid_argument("chroma edits require scene-linear working RGB");
}
void validate_curve(const PiecewiseLinearCurve& curve, std::size_t limit=256) {
    if (curve.knots.size()<2 || curve.knots.size()>limit)
        throw std::invalid_argument("curve knot count exceeds selected operation bounds");
    for (std::size_t i=0;i<curve.knots.size();++i) {
        const auto& p=curve.knots[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x)>65536 || std::abs(p.y)>65536)
            throw std::invalid_argument("curve coordinates must be finite with magnitude <=65536");
        if (i) {
            const auto& previous=curve.knots[i-1];
            if (!(p.x>previous.x)) throw std::invalid_argument("curve x coordinates must increase strictly");
            const auto slope=(p.y-previous.y)/(p.x-previous.x);
            if (!std::isfinite(slope) || std::abs(slope)>65536)
                throw std::invalid_argument("curve slope must be finite with magnitude <=65536");
        }
    }
}
CurvesSettings levels_curves(const LevelsSettings& settings) {
    CurvesSettings curves;
    for (unsigned c=0;c<3;++c) {
        const auto& p=settings.channels[c];
        if (!(p.output_white>=p.output_black))
            throw std::invalid_argument("levels output endpoints must be nondecreasing");
        curves.channels[c].knots={{p.input_black,p.output_black},{p.input_white,p.output_white}};
    }
    validate_curves_settings(curves);
    return curves;
}
void validate_input_tile(const Tile& tile, Rect r, ImageDescriptor descriptor) {
    if (tile.bounds.x!=r.x || tile.bounds.y!=r.y || tile.bounds.width!=r.width || tile.bounds.height!=r.height ||
        tile.descriptor!=descriptor || tile.rgb.size()%3 || tile.rgb.size()/3!=std::uint64_t(r.width)*r.height)
        throw std::invalid_argument("curve input tile bounds, storage or descriptor differ");
}
CurvesSettings lut1d_curves(const Lut1DSettings& settings, std::size_t limit=256) {
    const auto count=settings.channels[0].size();
    if (count<2 || count>limit)
        throw std::invalid_argument("1D LUT sample count exceeds selected operation bounds");
    if (!std::isfinite(settings.input_min) || !std::isfinite(settings.input_max) ||
        std::abs(settings.input_min)>65536 || std::abs(settings.input_max)>65536 ||
        !(settings.input_max>settings.input_min))
        throw std::invalid_argument("1D LUT input endpoints must be bounded finite and increasing");
    CurvesSettings curves;
    const double span=settings.input_max-settings.input_min;
    for (unsigned c=0;c<3;++c) {
        if (settings.channels[c].size()!=count)
            throw std::invalid_argument("1D LUT channel sample counts must match");
        auto& knots=curves.channels[c].knots; knots.clear(); knots.reserve(count);
        for (std::size_t i=0;i<count;++i) {
            // Exact endpoints; interior grid order is frozen to processing v2.
            const double x=i==0 ? settings.input_min : i==count-1 ? settings.input_max :
                settings.input_min+span*(double(i)/double(count-1));
            knots.push_back({x,settings.channels[c][i]});
        }
    }
    for (const auto& curve:curves.channels) validate_curve(curve,limit);
    return curves;
}
std::array<std::vector<double>,3> lut3d_axes(const Lut3DSettings& settings, std::uint32_t limit=17) {
    const auto n=settings.size;
    if (n<2 || n>limit) throw std::invalid_argument("3D LUT size exceeds selected operation bounds");
    if (settings.values.size()!=std::size_t(3)*n*n*n)
        throw std::invalid_argument("3D LUT needs exactly 3*size^3 values");
    std::array<std::vector<double>,3> axes;
    for (unsigned c=0;c<3;++c) {
        const double a=settings.input_min[c],b=settings.input_max[c];
        if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a)>65536 || std::abs(b)>65536 || !(b>a))
            throw std::invalid_argument("3D LUT endpoints must be bounded finite and increasing");
        const double span=b-a;
        for (unsigned i=0;i<n;++i) {
            const double x=i==0 ? a : i==n-1 ? b : a+span*(double(i)/double(n-1));
            if (i && !(x>axes[c].back())) throw std::invalid_argument("3D LUT grid must increase strictly");
            axes[c].push_back(x);
        }
    }
    for (double v:settings.values)
        if (!std::isfinite(v) || std::abs(v)>65536)
            throw std::invalid_argument("3D LUT values must be finite with magnitude <=65536");
    // PERF-017: up to 313,632 component-edge checks for a size33 table. Parser,
    // signature and copied settings costs must be profiled separately from mapping.
    for (unsigned b=0;b<n;++b) for (unsigned g=0;g<n;++g) for (unsigned r=0;r<n;++r) {
        const std::array<unsigned,3> index{r,g,b};
        const auto offset=std::size_t(3)*((b*n+g)*n+r);
        const std::array<std::size_t,3> strides{3,3*n,3*n*n};
        for (unsigned axis=0;axis<3;++axis) if (index[axis]+1<n) {
            const double dx=axes[axis][index[axis]+1]-axes[axis][index[axis]];
            for (unsigned c=0;c<3;++c) {
                const double slope=(settings.values[offset+strides[axis]+c]-settings.values[offset+c])/dx;
                if (!std::isfinite(slope) || std::abs(slope)>65536)
                    throw std::invalid_argument("3D LUT edge slopes must be finite with magnitude <=65536");
            }
        }
    }
    return axes;
}
double lut3d_lerp(double a,double b,double t) {
    const double value=t==0 ? a : t==1 ? b : a==b ? a : t>1 ? b+(t-1)*(b-a) : a+t*(b-a);
    if (!std::isfinite(value)) throw std::invalid_argument("3D LUT intermediate must be finite");
    return value;
}
} // namespace

void validate_curves_settings(const CurvesSettings& settings) {
    for (const auto& curve:settings.channels) validate_curve(curve);
}
void validate_levels_settings(const LevelsSettings& settings) { (void)levels_curves(settings); }
void validate_lut1d_settings(const Lut1DSettings& settings) { (void)lut1d_curves(settings); }
void validate_lut3d_settings(const Lut3DSettings& settings) { (void)lut3d_axes(settings); }
void validate_large_lut1d_settings(const Lut1DSettings& settings) { (void)lut1d_curves(settings,4096); }
void validate_large_lut3d_settings(const Lut3DSettings& settings) { (void)lut3d_axes(settings,33); }
void validate_saturation_settings(const SaturationSettings& settings) {
    if (!std::isfinite(settings.amount) || settings.amount<0 || settings.amount>4)
        throw std::invalid_argument("saturation amount must be finite in [0,4]");
}
void validate_vibrance_settings(const VibranceSettings& settings) {
    if (!std::isfinite(settings.amount) || settings.amount<-1 || settings.amount>1)
        throw std::invalid_argument("vibrance amount must be finite in [-1,1]");
}
void validate_channel_mixer_settings(const ChannelMixerSettings& settings) {
    for (double coefficient:settings.matrix)
        if (!std::isfinite(coefficient) || std::abs(coefficient)>64)
            throw std::invalid_argument("channel mixer coefficients must be finite with magnitude <=64");
}

CurvesNode::CurvesNode(std::shared_ptr<const Node> input, CurvesSettings settings)
    : CurvesNode(std::move(input),std::move(settings),256) {}
CurvesNode::CurvesNode(std::shared_ptr<const Node> input, CurvesSettings settings, std::size_t limit)
    : input_(std::move(input)),settings_(std::move(settings)) {
    for (const auto& curve:settings_.channels) validate_curve(curve,limit);
    if (!input_ || (input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
                   input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)))
        throw std::invalid_argument("curves require scene-linear working RGB");
    for (unsigned c=0;c<3;++c) {
        const auto& knots=settings_.channels[c].knots;
        identity_[c]=std::all_of(knots.begin(),knots.end(),[](const auto& p) { return p.x==p.y; });
        for (std::size_t i=1;i<knots.size();++i)
            slopes_[c].push_back((knots[i].y-knots[i-1].y)/(knots[i].x-knots[i-1].x));
    }
}
bool CurvesNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect CurvesNode::input_region_level(Rect output, Rect, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("curves do not support this render level");
    return output;
}
Tile CurvesNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile CurvesNode::render_level(Rect bounds, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("curves do not support this render level");
    auto tile=input_->render_level(bounds,level);
    validate_input_tile(tile,bounds,output_descriptor());
    for (std::size_t i=0;i<tile.rgb.size();++i) {
        const float sample=tile.rgb[i];
        if (!std::isfinite(sample)) throw std::invalid_argument("curves require finite input samples");
        const auto c=i%3;
        if (identity_[c]) continue;
        const auto& knots=settings_.channels[c].knots;
        const double value=sample;
        const auto next=std::upper_bound(knots.begin(),knots.end(),value,
            [](double x,const CurvePoint& p) { return x<p.x; });
        const auto left=next==knots.begin() ? std::size_t{0} : std::size_t(next-knots.begin()-1);
        const auto segment=std::min(left,knots.size()-2);
        // Anchor upper extrapolation at the last knot; exact knots emit their
        // saved y without interpolation rounding. No hidden endpoint clamp.
        const auto& anchor=knots[left];
        const double mapped=value==anchor.x ? anchor.y : anchor.y+(value-anchor.x)*slopes_[c][segment];
        if (!std::isfinite(mapped) || std::abs(mapped)>std::numeric_limits<float>::max())
            throw std::invalid_argument("curve output exceeds finite float32 range");
        tile.rgb[i]=static_cast<float>(mapped);
    }
    return tile;
}
LevelsNode::LevelsNode(std::shared_ptr<const Node> input, LevelsSettings settings)
    : curves_(std::move(input),levels_curves(settings)) {}
Lut1DNode::Lut1DNode(std::shared_ptr<const Node> input, Lut1DSettings settings)
    : curves_(std::move(input),lut1d_curves(settings)) {}

Lut3DNode::Lut3DNode(std::shared_ptr<const Node> input, Lut3DSettings settings)
    : Lut3DNode(std::move(input),std::move(settings),17) {}
Lut3DNode::Lut3DNode(std::shared_ptr<const Node> input, Lut3DSettings settings, std::uint32_t limit)
    : input_(std::move(input)),settings_(std::move(settings)),axes_(lut3d_axes(settings_,limit)) {
    if (!input_ || (input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
                   input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)))
        throw std::invalid_argument("3D LUT requires scene-linear working RGB");
    const auto n=settings_.size;
    for (unsigned b=0;b<n;++b) for (unsigned g=0;g<n;++g) for (unsigned r=0;r<n;++r) {
        const std::array<unsigned,3> index{r,g,b};
        const auto offset=std::size_t(3)*((b*n+g)*n+r);
        for (unsigned c=0;c<3;++c)
            identity_[c]=identity_[c] && settings_.values[offset+c]==axes_[c][index[c]];
    }
}
bool Lut3DNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect Lut3DNode::input_region_level(Rect output, Rect, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("3D LUT does not support this render level");
    return output;
}
Tile Lut3DNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile Lut3DNode::render_level(Rect bounds, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("3D LUT does not support this render level");
    auto tile=input_->render_level(bounds,level);
    validate_input_tile(tile,bounds,output_descriptor());
    const auto n=settings_.size;
    for (std::size_t i=0;i<tile.rgb.size();i+=3) {
        const std::array<float,3> samples{tile.rgb[i],tile.rgb[i+1],tile.rgb[i+2]};
        for (float sample:samples)
            if (!std::isfinite(sample)) throw std::invalid_argument("3D LUT requires finite input samples");
        if (identity_[0] && identity_[1] && identity_[2]) continue;
        std::array<std::size_t,3> left;
        std::array<double,3> fraction;
        for (unsigned c=0;c<3;++c) {
            const auto& axis=axes_[c];
            const auto next=std::upper_bound(axis.begin(),axis.end(),double(samples[c]));
            left[c]=next==axis.begin() ? 0 : std::min(std::size_t(next-axis.begin()-1),std::size_t(n-2));
            fraction[c]=(double(samples[c])-axis[left[c]])/(axis[left[c]+1]-axis[left[c]]);
            if (!std::isfinite(fraction[c])) throw std::invalid_argument("3D LUT fraction must be finite");
        }
        std::array<float,3> output;
        for (unsigned c=0;c<3;++c) {
            if (identity_[c]) { output[c]=samples[c]; continue; }
            std::array<double,2> planes;
            for (unsigned b=0;b<2;++b) {
                std::array<double,2> rows;
                for (unsigned g=0;g<2;++g) {
                    const auto offset=3*(((left[2]+b)*n+left[1]+g)*n+left[0])+c;
                    rows[g]=lut3d_lerp(settings_.values[offset],settings_.values[offset+3],fraction[0]);
                }
                planes[b]=lut3d_lerp(rows[0],rows[1],fraction[1]);
            }
            const double mapped=lut3d_lerp(planes[0],planes[1],fraction[2]);
            if (std::abs(mapped)>std::numeric_limits<float>::max())
                throw std::invalid_argument("3D LUT output exceeds finite float32 range");
            output[c]=static_cast<float>(mapped);
        }
        for (unsigned c=0;c<3;++c) tile.rgb[i+c]=output[c];
    }
    return tile;
}

SaturationNode::SaturationNode(std::shared_ptr<const Node> input, SaturationSettings settings)
    : input_(std::move(input)),settings_(settings) {
    validate_saturation_settings(settings_);
    if (!input_) throw std::invalid_argument("saturation needs input");
    // Y row derived from the same primary/white chromaticities as RawEngine.cpp,
    // rounded once to binary64 and pinned to this operation's processing version.
    const auto weights=working_y_weights(input_->output_descriptor());
    weight_red_=weights[0]; weight_blue_=weights[1];
}
bool SaturationNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect SaturationNode::input_region_level(Rect output, Rect, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("saturation does not support this render level");
    return output;
}
Tile SaturationNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile SaturationNode::render_level(Rect bounds, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("saturation does not support this render level");
    auto tile=input_->render_level(bounds,level);
    validate_input_tile(tile,bounds,output_descriptor());
    for (std::size_t i=0;i<tile.rgb.size();i+=3) {
        const double r=tile.rgb[i],g=tile.rgb[i+1],b=tile.rgb[i+2];
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
            throw std::invalid_argument("saturation requires finite input samples");
        // These exact bypasses retain signed zeros and extreme finite neutrals.
        if (settings_.amount==1 || (r==g && g==b)) continue;
        const double y=(g+weight_red_*(r-g))+weight_blue_*(b-g);
        std::array<double,3> mapped;
        for (unsigned c=0;c<3;++c) {
            mapped[c]=y+settings_.amount*(double(tile.rgb[i+c])-y);
            if (!std::isfinite(mapped[c]) || std::abs(mapped[c])>std::numeric_limits<float>::max())
                throw std::invalid_argument("saturation output exceeds finite float32 range");
        }
        for (unsigned c=0;c<3;++c) tile.rgb[i+c]=static_cast<float>(mapped[c]);
    }
    return tile;
}

VibranceNode::VibranceNode(std::shared_ptr<const Node> input, VibranceSettings settings)
    : input_(std::move(input)),settings_(settings) {
    validate_vibrance_settings(settings_);
    if (!input_) throw std::invalid_argument("vibrance needs input");
    const auto weights=working_y_weights(input_->output_descriptor());
    weight_red_=weights[0]; weight_blue_=weights[1];
}
bool VibranceNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect VibranceNode::input_region_level(Rect output, Rect, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("vibrance does not support this render level");
    return output;
}
Tile VibranceNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile VibranceNode::render_level(Rect bounds, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("vibrance does not support this render level");
    auto tile=input_->render_level(bounds,level);
    validate_input_tile(tile,bounds,output_descriptor());
    for (std::size_t i=0;i<tile.rgb.size();i+=3) {
        const double r=tile.rgb[i],g=tile.rgb[i+1],b=tile.rgb[i+2];
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
            throw std::invalid_argument("vibrance requires finite input samples");
        if (settings_.amount==0 || (r==g && g==b)) continue;
        const double y=(g+weight_red_*(r-g))+weight_blue_*(b-g);
        const double delta=std::max({r,g,b})-std::min({r,g,b});
        const double a=std::abs(y);
        const double weight=a/(a+delta);
        const double scale=1+settings_.amount*weight;
        // Includes exact zero-Y colors and rounded unit scale. Preserve bits.
        if (scale==1) continue;
        std::array<double,3> mapped;
        for (unsigned c=0;c<3;++c) {
            mapped[c]=y+scale*(double(tile.rgb[i+c])-y);
            if (!std::isfinite(mapped[c]) || std::abs(mapped[c])>std::numeric_limits<float>::max())
                throw std::invalid_argument("vibrance output exceeds finite float32 range");
        }
        for (unsigned c=0;c<3;++c) tile.rgb[i+c]=static_cast<float>(mapped[c]);
    }
    return tile;
}

ChannelMixerNode::ChannelMixerNode(std::shared_ptr<const Node> input, ChannelMixerSettings settings)
    : input_(std::move(input)),settings_(settings) {
    validate_channel_mixer_settings(settings_);
    if (!input_ || (input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
                   input_->output_descriptor()!=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)))
        throw std::invalid_argument("channel mixer requires scene-linear working RGB");
    for (unsigned row=0;row<3;++row) for (unsigned channel=0;channel<3;++channel) {
        bool unit=true;
        for (unsigned c=0;c<3;++c)
            unit=unit && settings_.matrix[row*3+c]==(c==channel ? 1 : 0);
        if (unit) selected_channel_[row]=int(channel);
    }
}
bool ChannelMixerNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect ChannelMixerNode::input_region_level(Rect output, Rect, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("channel mixer does not support this render level");
    return output;
}
Tile ChannelMixerNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile ChannelMixerNode::render_level(Rect bounds, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("channel mixer does not support this render level");
    auto tile=input_->render_level(bounds,level);
    validate_input_tile(tile,bounds,output_descriptor());
    for (std::size_t i=0;i<tile.rgb.size();i+=3) {
        const std::array<float,3> samples{tile.rgb[i],tile.rgb[i+1],tile.rgb[i+2]};
        for (float sample:samples)
            if (!std::isfinite(sample)) throw std::invalid_argument("channel mixer requires finite input samples");
        std::array<float,3> output;
        for (unsigned row=0;row<3;++row) {
            if (selected_channel_[row]>=0) {
                output[row]=samples[unsigned(selected_channel_[row])];
                continue;
            }
            const auto* m=settings_.matrix.data()+row*3;
            const double mapped=(m[0]*double(samples[0])+m[1]*double(samples[1]))+m[2]*double(samples[2]);
            if (!std::isfinite(mapped) || std::abs(mapped)>std::numeric_limits<float>::max())
                throw std::invalid_argument("channel mixer output exceeds finite float32 range");
            output[row]=static_cast<float>(mapped);
        }
        for (unsigned row=0;row<3;++row) tile.rgb[i+row]=output[row];
    }
    return tile;
}

void validate_color_mixer_settings(const ColorMixerSettings& settings) {
    for (unsigned i=0;i<8;++i)
        if (!std::isfinite(settings.hue_shift[i]) || std::abs(settings.hue_shift[i])>60 ||
            !std::isfinite(settings.saturation_delta[i]) || std::abs(settings.saturation_delta[i])>1 ||
            !std::isfinite(settings.luminance_delta[i]) || std::abs(settings.luminance_delta[i])>1)
            throw std::invalid_argument("color mixer needs finite hue +/-60 and chroma/luminance +/-1");
}
ColorMixerNode::ColorMixerNode(std::shared_ptr<const Node> input, ColorMixerSettings settings)
    : input_(std::move(input)),settings_(settings) {
    validate_color_mixer_settings(settings_);
    if (!input_) throw std::invalid_argument("color mixer needs input");
    const auto weights=working_y_weights(input_->output_descriptor());
    weight_red_=weights[0]; weight_blue_=weights[1];
    for (unsigned i=0;i<8;++i)
        identity_=identity_ && settings_.hue_shift[i]==0 && settings_.saturation_delta[i]==0 && settings_.luminance_delta[i]==0;
}
bool ColorMixerNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect ColorMixerNode::input_region_level(Rect output, Rect, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("color mixer does not support this render level");
    return output;
}
Tile ColorMixerNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile ColorMixerNode::render_level(Rect bounds, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("color mixer does not support this render level");
    auto tile=input_->render_level(bounds,level);
    validate_input_tile(tile,bounds,output_descriptor());
    constexpr std::array<double,9> centers{0,30,60,120,180,240,270,300,360};
    const auto wrap=[](double h) {
        if (h<0) h+=360;
        if (h>=360) h-=360;
        return h==0 ? 0. : h;
    };
    const auto blend=[](double a,double b,double t) {
        if (t==0) return a;
        if (t==1) return b;
        if (a==b) return a;
        return (1-t)*a+t*b;
    };
    const auto y=[this](const std::array<double,3>& v) {
        return (v[1]+weight_red_*(v[0]-v[1]))+weight_blue_*(v[2]-v[1]);
    };
    for (std::size_t i=0;i<tile.rgb.size();i+=3) {
        const std::array<double,3> rgb{tile.rgb[i],tile.rgb[i+1],tile.rgb[i+2]};
        for (double sample:rgb)
            if (!std::isfinite(sample)) throw std::invalid_argument("color mixer requires finite input samples");
        if (identity_ || (rgb[0]==rgb[1] && rgb[1]==rgb[2])) continue;
        const double maximum=std::max({rgb[0],rgb[1],rgb[2]});
        const double chroma=maximum-std::min({rgb[0],rgb[1],rgb[2]});
        double h=maximum==rgb[0] ? 60*((rgb[1]-rgb[2])/chroma) :
                 maximum==rgb[1] ? 60*(((rgb[2]-rgb[0])/chroma)+2) : 60*(((rgb[0]-rgb[1])/chroma)+4);
        h=wrap(h);
        const auto band=std::size_t(std::upper_bound(centers.begin(),centers.end(),h)-centers.begin()-1);
        const double t=(h-centers[band])/(centers[band+1]-centers[band]);
        const auto next=(band+1)%8;
        const double shift=blend(settings_.hue_shift[band],settings_.hue_shift[next],t);
        const double delta=blend(settings_.saturation_delta[band],settings_.saturation_delta[next],t);
        const double luma=blend(settings_.luminance_delta[band],settings_.luminance_delta[next],t);
        const double original_y=y(rgb);
        const double fade=chroma/(std::abs(original_y)+chroma);
        const double scale=1+delta;
        const double factor=1+luma*fade;
        const double target=original_y*factor;
        if (!std::isfinite(h) || !std::isfinite(t) || !std::isfinite(original_y) || !std::isfinite(fade) ||
            !std::isfinite(scale) || !std::isfinite(target))
            throw std::invalid_argument("color mixer nonfinite intermediate");
        if (shift==0 && scale==1 && target==original_y) continue;
        std::array<double,3> centered;
        if (shift==0) {
            for (unsigned c=0;c<3;++c) centered[c]=rgb[c]-original_y;
        } else {
            const double u=wrap(h+shift)/60;
            const auto sector=unsigned(std::floor(u));
            const double f=u-sector;
            std::array<double,3> q;
            switch (sector) {
                case 0: q={chroma,chroma*f,0}; break;
                case 1: q={chroma*(1-f),chroma,0}; break;
                case 2: q={0,chroma,chroma*f}; break;
                case 3: q={0,chroma*(1-f),chroma}; break;
                case 4: q={chroma*f,0,chroma}; break;
                default: q={chroma,0,chroma*(1-f)}; break;
            }
            const double qy=y(q);
            for (unsigned c=0;c<3;++c) centered[c]=q[c]-qy;
        }
        std::array<double,3> mapped;
        for (unsigned c=0;c<3;++c) {
            mapped[c]=target+scale*centered[c];
            if (!std::isfinite(centered[c]) || !std::isfinite(mapped[c]) || std::abs(mapped[c])>std::numeric_limits<float>::max())
                throw std::invalid_argument("color mixer output exceeds finite float32 range");
        }
        for (unsigned c=0;c<3;++c) tile.rgb[i+c]=static_cast<float>(mapped[c]);
    }
    return tile;
}

void validate_color_balance_settings(const ColorBalanceSettings& settings) {
    for (unsigned c=0;c<3;++c)
        if (!std::isfinite(settings.shadows[c]) || std::abs(settings.shadows[c])>1 ||
            !std::isfinite(settings.midtones[c]) || std::abs(settings.midtones[c])>1 ||
            !std::isfinite(settings.highlights[c]) || std::abs(settings.highlights[c])>1)
            throw std::invalid_argument("color balance needs finite RGB offsets +/-1");
}
ColorBalanceNode::ColorBalanceNode(std::shared_ptr<const Node> input, ColorBalanceSettings settings)
    : input_(std::move(input)),settings_(settings) {
    validate_color_balance_settings(settings_);
    if (!input_) throw std::invalid_argument("color balance needs input");
    const auto weights=working_y_weights(input_->output_descriptor());
    weight_red_=weights[0]; weight_blue_=weights[1];
    for (unsigned c=0;c<3;++c)
        identity_=identity_ && settings_.shadows[c]==0 && settings_.midtones[c]==0 && settings_.highlights[c]==0;
}
bool ColorBalanceNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect ColorBalanceNode::input_region_level(Rect output, Rect, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("color balance does not support this render level");
    return output;
}
Tile ColorBalanceNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile ColorBalanceNode::render_level(Rect bounds, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("color balance does not support this render level");
    auto tile=input_->render_level(bounds,level);
    validate_input_tile(tile,bounds,output_descriptor());
    const auto y=[this](const std::array<double,3>& v) {
        return (v[1]+weight_red_*(v[0]-v[1]))+weight_blue_*(v[2]-v[1]);
    };
    for (std::size_t i=0;i<tile.rgb.size();i+=3) {
        const std::array<double,3> rgb{tile.rgb[i],tile.rgb[i+1],tile.rgb[i+2]};
        for (double sample:rgb)
            if (!std::isfinite(sample)) throw std::invalid_argument("color balance requires finite input samples");
        if (identity_) continue;
        const double original_y=y(rgb);
        const double t=original_y<=0 ? 0 : original_y>=1 ? 1 : original_y;
        const double a=1-t,ws=a*a,wm=(2*t)*a,wh=t*t;
        if (!std::isfinite(original_y) || !std::isfinite(t) || !std::isfinite(ws) || !std::isfinite(wm) || !std::isfinite(wh))
            throw std::invalid_argument("color balance nonfinite tonal weights");
        std::array<double,3> delta;
        for (unsigned c=0;c<3;++c) {
            const double s=settings_.shadows[c],m=settings_.midtones[c],h=settings_.highlights[c];
            delta[c]=t==0 ? s : t==1 ? h : (s==m && m==h) ? s : (ws*s+wm*m)+wh*h;
            if (!std::isfinite(delta[c])) throw std::invalid_argument("color balance nonfinite blended offset");
        }
        if (delta[0]==0 && delta[1]==0 && delta[2]==0) continue;
        const double delta_y=settings_.preserve_luminance ? y(delta) : 0;
        if (!std::isfinite(delta_y)) throw std::invalid_argument("color balance nonfinite offset luminance");
        std::array<double,3> offset,mapped;
        for (unsigned c=0;c<3;++c) {
            offset[c]=settings_.preserve_luminance ? delta[c]-delta_y : delta[c];
            mapped[c]=offset[c]==0 ? rgb[c] : rgb[c]+offset[c];
            if (!std::isfinite(offset[c]) || !std::isfinite(mapped[c]) || std::abs(mapped[c])>std::numeric_limits<float>::max())
                throw std::invalid_argument("color balance output exceeds finite float32 range");
        }
        for (unsigned c=0;c<3;++c)
            if (offset[c]!=0) tile.rgb[i+c]=static_cast<float>(mapped[c]);
    }
    return tile;
}

GrayscaleNode::GrayscaleNode(std::shared_ptr<const Node> input) : input_(std::move(input)) {
    if (!input_) throw std::invalid_argument("grayscale needs input");
    const auto weights=working_y_weights(input_->output_descriptor());
    weight_red_=weights[0]; weight_blue_=weights[1];
}
bool GrayscaleNode::supports_level(RenderLevel level) const noexcept {
    return ((level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
            (level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview)) && input_->supports_level(level);
}
Rect GrayscaleNode::input_region_level(Rect output, Rect, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("grayscale does not support this render level");
    return output;
}
Tile GrayscaleNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile GrayscaleNode::render_level(Rect bounds, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("grayscale does not support this render level");
    auto tile=input_->render_level(bounds,level);
    validate_input_tile(tile,bounds,output_descriptor());
    for (std::size_t i=0;i<tile.rgb.size();i+=3) {
        const double r=tile.rgb[i],g=tile.rgb[i+1],b=tile.rgb[i+2];
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
            throw std::invalid_argument("grayscale requires finite input samples");
        if (r==g && g==b) continue;
        const double y=(g+weight_red_*(r-g))+weight_blue_*(b-g);
        if (!std::isfinite(y) || std::abs(y)>std::numeric_limits<float>::max())
            throw std::invalid_argument("grayscale output exceeds finite float32 range");
        const float gray=static_cast<float>(y);
        tile.rgb[i]=gray; tile.rgb[i+1]=gray; tile.rgb[i+2]=gray;
    }
    return tile;
}

LargeLut1DNode::LargeLut1DNode(std::shared_ptr<const Node> input, Lut1DSettings settings)
    : curves_(std::move(input),lut1d_curves(settings,4096),4096) {}
LargeLut3DNode::LargeLut3DNode(std::shared_ptr<const Node> input, Lut3DSettings settings)
    : node_(std::move(input),std::move(settings),33) {}
void validate_grading_settings(const GradingSettings& s) {
    for (unsigned c=0;c<3;++c)
        if (!std::isfinite(s.lift[c]) || std::abs(s.lift[c])>1 ||
            !std::isfinite(s.gain[c]) || s.gain[c]<0 || s.gain[c]>4 ||
            !std::isfinite(s.gamma[c]) || s.gamma[c]<.25 || s.gamma[c]>4)
            throw std::invalid_argument("grading needs bounded finite lift/gain/gamma");
}
GradingNode::GradingNode(std::shared_ptr<const Node> input, GradingSettings settings)
    : input_(std::move(input)),settings_(settings) {
    validate_grading_settings(settings_);
    if (!input_) throw std::invalid_argument("grading needs input");
    (void)working_y_weights(input_->output_descriptor());
}
bool GradingNode::supports_level(RenderLevel l) const noexcept {
    return ((l.mip==0 && (l.quality==RenderQuality::Final || l.quality==RenderQuality::Preview)) ||
            (l.mip>=1 && l.mip<=2 && l.quality==RenderQuality::Preview)) && input_->supports_level(l);
}
Rect GradingNode::input_region_level(Rect r, Rect, RenderLevel l) const {
    if (!supports_level(l)) throw std::invalid_argument("grading does not support this render level");
    return r;
}
Tile GradingNode::render(Rect r) const { return render_level(r,{}); }
Tile GradingNode::render_level(Rect r, RenderLevel l) const {
    if (!supports_level(l)) throw std::invalid_argument("grading does not support this render level");
    auto tile=input_->render_level(r,l);validate_input_tile(tile,r,output_descriptor());
    for (std::size_t i=0;i<tile.rgb.size();++i) {
        const double x=tile.rgb[i];
        if (!std::isfinite(x)) throw std::invalid_argument("grading requires finite input samples");
        const auto c=i%3;const double lift=settings_.lift[c],gain=settings_.gain[c],gamma=settings_.gamma[c];
        if (lift==0 && gain==1 && gamma==1) continue;
        const double scaled=gain==1 ? x : x*gain;
        const double u=lift==0 ? scaled : scaled+lift;
        const double y=gamma==1 ? u : std::copysign(std::pow(std::abs(u),1/gamma),u);
        if (!std::isfinite(y) || std::abs(y)>std::numeric_limits<float>::max())
            throw std::invalid_argument("grading output exceeds finite float32 range");
        tile.rgb[i]=static_cast<float>(y);
    }
    return tile;
}

} // namespace rawengine
