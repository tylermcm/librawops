#include "ParametricMask.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {

void coordinate(double value) {
    if (!std::isfinite(value) || value < -0x1p33 || value > 0x1p33)
        throw std::invalid_argument("parametric mask coordinate must be finite within +/-2^33");
}
void point(MaskPoint value) {coordinate(value.x);coordinate(value.y);}
void extent(const std::shared_ptr<const CoverageNode>& input) {
    if (!input) throw std::invalid_argument("parametric mask input is null");
    const auto bounds=input->native_bounds();
    constexpr auto maximum=std::numeric_limits<std::uint32_t>::max();
    if (!bounds.width || !bounds.height || bounds.x>maximum-bounds.width || bounds.y>maximum-bounds.height)
        throw std::invalid_argument("parametric mask extent is empty or overflows");
}
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}
bool admitted(RenderLevel level) {
    return (level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
           ((level.mip==1 || level.mip==2) && level.quality==RenderQuality::Preview);
}
RenderLevel native_level(const CoverageNode& node,RenderLevel level) {
    if (!node.supports_level(level)) throw std::invalid_argument("unsupported parametric mask level");
    return {0,RenderQuality::Final};
}
Rect native_region(const CoverageNode& node,Rect output,Rect input_bounds,RenderLevel level) {
    if (!same(input_bounds,node.native_bounds())) throw std::invalid_argument("parametric mask input extent mismatch");
    const auto bounds=node.output_bounds(level);
    if (!output.width || !output.height || output.x<bounds.x || output.y<bounds.y ||
        output.width>bounds.width || output.height>bounds.height ||
        output.x-bounds.x>bounds.width-output.width || output.y-bounds.y>bounds.height-output.height)
        throw std::invalid_argument("parametric mask ROI is outside its level extent");
    if (!level.mip) return output;
    const auto native=node.native_bounds();const std::uint64_t scale=1u<<level.mip;
    const auto x=static_cast<std::uint64_t>(output.x)*scale,y=static_cast<std::uint64_t>(output.y)*scale;
    const auto right=std::min((static_cast<std::uint64_t>(output.x)+output.width)*scale,static_cast<std::uint64_t>(native.width));
    const auto bottom=std::min((static_cast<std::uint64_t>(output.y)+output.height)*scale,static_cast<std::uint64_t>(native.height));
    return {static_cast<std::uint32_t>(native.x+x),static_cast<std::uint32_t>(native.y+y),
            static_cast<std::uint32_t>(right-x),static_cast<std::uint32_t>(bottom-y)};
}
std::size_t count(Rect bounds) {
    const auto pixels=static_cast<std::uint64_t>(bounds.width)*bounds.height;
    if (pixels>std::numeric_limits<std::size_t>::max()/sizeof(float) || pixels>std::vector<float>{}.max_size())
        throw std::invalid_argument("parametric mask storage capacity overflows");
    return static_cast<std::size_t>(pixels);
}
float store(double value,bool invert) {
    const float shape=value==0?0.0f:static_cast<float>(value);
    return invert?invert_coverage(shape):shape;
}
std::array<double,4> inverse(const RadialGradientMaskSettings& settings) {
    const auto& axes=settings.axes;
    const double p=axes[0]*axes[3],q=axes[2]*axes[1],det=p-q;
    if (det==0) throw std::invalid_argument("radial mask axes are singular");
    std::array<double,4> coefficients{axes[3]/det,(-axes[2])/det,(-axes[1])/det,axes[0]/det};
    for (double coefficient:coefficients)
        if (!std::isfinite(coefficient) || std::abs(coefficient)>256)
            throw std::invalid_argument("radial mask inverse amplification exceeds 256");
    return coefficients;
}
float linear(double x,double y,const LinearGradientMaskSettings& settings) {
    const double vx=settings.end.x-settings.start.x,vy=settings.end.y-settings.start.y;
    const double sx=vx*vx,sy=vy*vy,den=sx+sy;
    const double dx=x-settings.start.x,dy=y-settings.start.y;
    const double nx=dx*vx,ny=dy*vy,num=nx+ny;
    return store(num<=0?0.0:num>=den?1.0:num/den,settings.invert);
}
float radial(double x,double y,const RadialGradientMaskSettings& settings) {
    const auto coefficients=inverse(settings);
    const double dx=x-settings.center.x,dy=y-settings.center.y;
    const double au=coefficients[0]*dx,bv=coefficients[1]*dy;
    const double cu=coefficients[2]*dx,dv=coefficients[3]*dy;
    const double u=au+bv,v=cu+dv,uu=u*u,vv=v*v,r2=uu+vv;
    const double inner2=settings.inner*settings.inner;
    double value;
    if (r2>=1) value=0;
    else if (r2<=inner2) value=1;
    else {
        const double numerator=1.0-r2,denominator=1.0-inner2;
        value=std::clamp(numerator/denominator,0.0,1.0);
    }
    return store(value,settings.invert);
}
float polygon(double x,double y,const PolygonMaskSettings& settings) {
    bool parity=false;
    for (const auto& ring:settings.rings) {
        for (std::size_t i=0;i<ring.size();++i) {
            const auto a=ring[i],b=ring[(i+1)%ring.size()];
            const double dx=x-a.x,dy=y-a.y,vx=b.x-a.x,vy=b.y-a.y;
            const double left=dx*vy,right=dy*vx,cross=left-right;
            if (cross==0 && x>=std::min(a.x,b.x) && x<=std::max(a.x,b.x) &&
                y>=std::min(a.y,b.y) && y<=std::max(a.y,b.y)) return store(1,settings.invert);
            if ((a.y>y)!=(b.y>y)) {
                double intersection;
                if (y==a.y) intersection=a.x;
                else if (y==b.y) intersection=b.x;
                else {
                    const double num=y-a.y,den=b.y-a.y,t=num/den;
                    const double offset=t*(b.x-a.x);
                    intersection=a.x+offset;
                }
                if (x<intersection) parity=!parity;
            }
        }
    }
    return store(parity?1.0:0.0,settings.invert);
}
template<class Evaluator> CoverageTile render_shape(const CoverageNode& node,const CoverageNode& input,
    Rect bounds,RenderLevel level,Evaluator evaluate) {
    const auto request=native_region(node,bounds,input.native_bounds(),level);
    (void)count(request);(void)count(bounds);
    auto native=input.render_level(request,native_level(node,level));
    if (!same(native.bounds,request)) throw std::domain_error("parametric mask input returned wrong ROI");
    validate_coverage_tile(native);
    for (std::uint32_t y=0;y<request.height;++y) for (std::uint32_t x=0;x<request.width;++x) {
        const auto i=static_cast<std::size_t>(y)*request.width+x;
        const float shape=evaluate(static_cast<double>(request.x)+x+0.5,static_cast<double>(request.y)+y+0.5);
        if (shape==0) native.coverage[i]=0;
        else if (shape!=1) {
            const double mapped=static_cast<double>(native.coverage[i])*static_cast<double>(shape);
            native.coverage[i]=mapped==0?0.0f:static_cast<float>(mapped);
        }
    }
    if (!level.mip) return native;
    CoverageTile output{bounds,std::vector<float>(count(bounds))};const auto scale=1u<<level.mip;
    for (std::uint32_t y=0;y<bounds.height;++y) for (std::uint32_t x=0;x<bounds.width;++x) {
        const auto sx=static_cast<std::uint64_t>(x)*scale,sy=static_cast<std::uint64_t>(y)*scale;
        const auto ex=std::min(sx+scale,static_cast<std::uint64_t>(request.width));
        const auto ey=std::min(sy+scale,static_cast<std::uint64_t>(request.height));
        double sum=0;
        for (auto v=sy;v<ey;++v) for (auto u=sx;u<ex;++u)
            sum+=static_cast<double>(native.coverage[static_cast<std::size_t>(v)*request.width+u]);
        const double samples=static_cast<double>((ex-sx)*(ey-sy));
        output.coverage[static_cast<std::size_t>(y)*bounds.width+x]=static_cast<float>(sum/samples);
    }
    return output;
}

} // namespace

void validate_linear_gradient_mask_settings(const LinearGradientMaskSettings& settings) {
    point(settings.start);point(settings.end);
    const double vx=settings.end.x-settings.start.x,vy=settings.end.y-settings.start.y;
    const double sx=vx*vx,sy=vy*vy,den=sx+sy;
    if (den<0x1p-16) throw std::invalid_argument("linear mask segment is shorter than 2^-8");
}
void validate_radial_gradient_mask_settings(const RadialGradientMaskSettings& settings) {
    point(settings.center);for (double axis:settings.axes) coordinate(axis);
    if (!std::isfinite(settings.inner) || settings.inner<0 || settings.inner>1)
        throw std::invalid_argument("radial mask inner must be finite in [0,1]");
    (void)inverse(settings);
}
void validate_polygon_mask_settings(const PolygonMaskSettings& settings) {
    if (settings.rings.empty() || settings.rings.size()>4096) throw std::invalid_argument("invalid polygon ring count");
    std::size_t total=0;
    for (const auto& ring:settings.rings) {
        if (ring.size()<3 || ring.size()>65536 || ring.size()>1048576-total)
            throw std::invalid_argument("invalid polygon point count");
        total+=ring.size();for (auto value:ring) point(value);
    }
}
float evaluate_linear_gradient_mask(double x,double y,const LinearGradientMaskSettings& settings) {
    coordinate(x);coordinate(y);validate_linear_gradient_mask_settings(settings);return linear(x,y,settings);
}
float evaluate_radial_gradient_mask(double x,double y,const RadialGradientMaskSettings& settings) {
    coordinate(x);coordinate(y);validate_radial_gradient_mask_settings(settings);return radial(x,y,settings);
}
float evaluate_polygon_mask(double x,double y,const PolygonMaskSettings& settings) {
    coordinate(x);coordinate(y);validate_polygon_mask_settings(settings);return polygon(x,y,settings);
}

CoverageLinearGradientNode::CoverageLinearGradientNode(std::shared_ptr<const CoverageNode> input,const LinearGradientMaskSettings& settings)
    :input_(std::move(input)) {extent(input_);validate_linear_gradient_mask_settings(settings);settings_=settings;}
bool CoverageLinearGradientNode::supports_level(RenderLevel level) const noexcept {return admitted(level)&&input_->supports_level({});}
Rect CoverageLinearGradientNode::native_bounds() const noexcept {return input_->native_bounds();}
RenderLevel CoverageLinearGradientNode::input_level(RenderLevel level) const {return native_level(*this,level);}
Rect CoverageLinearGradientNode::input_region_level(Rect output,Rect input_bounds,RenderLevel level) const {return native_region(*this,output,input_bounds,level);}
CoverageTile CoverageLinearGradientNode::render_level(Rect bounds,RenderLevel level) const {
    return render_shape(*this,*input_,bounds,level,[&](double x,double y) {return linear(x,y,settings_);});
}
CoverageRadialGradientNode::CoverageRadialGradientNode(std::shared_ptr<const CoverageNode> input,const RadialGradientMaskSettings& settings)
    :input_(std::move(input)) {extent(input_);validate_radial_gradient_mask_settings(settings);settings_=settings;}
bool CoverageRadialGradientNode::supports_level(RenderLevel level) const noexcept {return admitted(level)&&input_->supports_level({});}
Rect CoverageRadialGradientNode::native_bounds() const noexcept {return input_->native_bounds();}
RenderLevel CoverageRadialGradientNode::input_level(RenderLevel level) const {return native_level(*this,level);}
Rect CoverageRadialGradientNode::input_region_level(Rect output,Rect input_bounds,RenderLevel level) const {return native_region(*this,output,input_bounds,level);}
CoverageTile CoverageRadialGradientNode::render_level(Rect bounds,RenderLevel level) const {
    return render_shape(*this,*input_,bounds,level,[&](double x,double y) {return radial(x,y,settings_);});
}
CoveragePolygonNode::CoveragePolygonNode(std::shared_ptr<const CoverageNode> input,const PolygonMaskSettings& settings)
    :input_(std::move(input)) {extent(input_);validate_polygon_mask_settings(settings);settings_=settings;}
bool CoveragePolygonNode::supports_level(RenderLevel level) const noexcept {return admitted(level)&&input_->supports_level({});}
Rect CoveragePolygonNode::native_bounds() const noexcept {return input_->native_bounds();}
RenderLevel CoveragePolygonNode::input_level(RenderLevel level) const {return native_level(*this,level);}
Rect CoveragePolygonNode::input_region_level(Rect output,Rect input_bounds,RenderLevel level) const {return native_region(*this,output,input_bounds,level);}
CoverageTile CoveragePolygonNode::render_level(Rect bounds,RenderLevel level) const {
    return render_shape(*this,*input_,bounds,level,[&](double x,double y) {return polygon(x,y,settings_);});
}

} // namespace rawengine
