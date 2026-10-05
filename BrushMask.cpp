#include "BrushMask.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {

constexpr double coordinate_limit = 0x1p33;

bool same(Rect a, Rect b) {
    return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
}

void extent(Rect bounds) {
    constexpr auto maximum=std::numeric_limits<std::uint32_t>::max();
    if (!bounds.width || !bounds.height || bounds.x>maximum-bounds.width || bounds.y>maximum-bounds.height)
        throw std::invalid_argument("brush mask extent is empty or overflows");
}

void coordinate(double value) {
    if (!std::isfinite(value) || value < -coordinate_limit || value > coordinate_limit)
        throw std::invalid_argument("brush coordinate must be finite and within +/-2^33");
}

void strength(double value) {
    if (!std::isfinite(value) || value<0 || value>1)
        throw std::invalid_argument("brush strength must be finite and in [0,1]");
}

bool admitted(RenderLevel level) {
    return (level.mip==0 && (level.quality==RenderQuality::Final || level.quality==RenderQuality::Preview)) ||
           ((level.mip==1 || level.mip==2) && level.quality==RenderQuality::Preview);
}

void contained(Rect bounds, Rect roi) {
    if (!roi.width || !roi.height || roi.x<bounds.x || roi.y<bounds.y ||
        roi.width>bounds.width || roi.height>bounds.height ||
        roi.x-bounds.x>bounds.width-roi.width || roi.y-bounds.y>bounds.height-roi.height)
        throw std::invalid_argument("brush ROI is outside its level extent");
}

std::size_t count(Rect roi) {
    const auto n=static_cast<std::uint64_t>(roi.width)*roi.height;
    if (n>std::numeric_limits<std::size_t>::max()/sizeof(float) || n>std::vector<float>{}.max_size())
        throw std::invalid_argument("brush mask storage capacity overflows");
    return static_cast<std::size_t>(n);
}

double primitive_strength(double px,double py,BrushPoint center,const BrushStroke& stroke) {
    const double qx=px-center.x,qy=py-center.y;
    const double sx=qx*qx,sy=qy*qy;
    const double q=sx+sy;
    const double r2=stroke.radius*stroke.radius;
    const double h2=stroke.hardness*stroke.hardness;
    const double inner2=h2*r2;
    double kernel;
    if (q>=r2) kernel=0;
    else if (q<=inner2) kernel=1;
    else {
        const double numerator=r2-q,denominator=r2-inner2;
        kernel=std::clamp(numerator/denominator,0.0,1.0);
    }
    return kernel*center.pressure;
}

BrushPoint segment_center(double px,double py,BrushPoint a,BrushPoint b) {
    const double vx=b.x-a.x,vy=b.y-a.y,dx=px-a.x,dy=py-a.y;
    const double vvx=vx*vx,vvy=vy*vy;
    const double den=vvx+vvy;
    if (den==0) return {a.x,a.y,std::max(a.pressure,b.pressure)};
    const double nx=dx*vx,ny=dy*vy;
    const double num=nx+ny;
    if (num<=0) return a;
    if (num>=den) return b;
    const double t=num/den;
    const double tx=t*vx,ty=t*vy;
    const double cx=a.x+tx,cy=a.y+ty;
    const double dp=b.pressure-a.pressure,tp=t*dp;
    const double pressure=std::clamp(a.pressure+tp,0.0,1.0);
    return {cx,cy,pressure};
}

float replay(float coverage,double px,double py,const BrushMaskSettings& settings) {
    for (const auto& stroke:settings.strokes) {
        double maximum=0;
        for (const auto& point:stroke.points)
            maximum=std::max(maximum,primitive_strength(px,py,point,stroke));
        for (std::size_t i=1;i<stroke.points.size();++i)
            maximum=std::max(maximum,primitive_strength(px,py,
                segment_center(px,py,stroke.points[i-1],stroke.points[i]),stroke));
        const double f=maximum*stroke.flow,w=f*stroke.opacity;
        const float alpha=static_cast<float>(w);
        if (alpha==0) continue;
        if (alpha==1) {coverage=stroke.mode==BrushMode::Paint?1.0f:0.0f;continue;}
        const double a=coverage,weight=alpha;
        double mapped;
        if (stroke.mode==BrushMode::Paint) {
            const double remaining=1.0-a,scaled=weight*remaining;
            mapped=a+scaled;
        } else {
            const double remaining=1.0-weight;
            mapped=a*remaining;
        }
        coverage=mapped==0?0.0f:static_cast<float>(mapped);
    }
    return coverage;
}

} // namespace

void validate_brush_mask_settings(const BrushMaskSettings& settings) {
    if (settings.strokes.size()>4096) throw std::invalid_argument("too many brush strokes");
    std::size_t total=0;
    for (const auto& stroke:settings.strokes) {
        if (stroke.mode!=BrushMode::Paint && stroke.mode!=BrushMode::Erase)
            throw std::invalid_argument("unsupported brush mode");
        if (!std::isfinite(stroke.radius) || stroke.radius<0x1p-8 || stroke.radius>0x1p20)
            throw std::invalid_argument("brush radius must be in [2^-8,2^20]");
        strength(stroke.hardness);strength(stroke.flow);strength(stroke.opacity);
        if (stroke.points.empty() || stroke.points.size()>65536 || stroke.points.size()>1048576-total)
            throw std::invalid_argument("brush point count exceeds admission limits");
        total+=stroke.points.size();
        for (const auto& point:stroke.points) {
            coordinate(point.x);coordinate(point.y);strength(point.pressure);
        }
    }
}

float apply_brush_mask(float coverage,double center_x,double center_y,const BrushMaskSettings& settings) {
    validate_brush_mask_settings(settings);coordinate(center_x);coordinate(center_y);
    const auto bits=std::bit_cast<std::uint32_t>(coverage),magnitude=bits&0x7fffffffu;
    if (magnitude>0x3f800000u || (magnitude && (bits&0x80000000u)))
        throw std::invalid_argument("brush input coverage must be finite and in [0,1]");
    return replay(coverage,center_x,center_y,settings);
}

CoverageBrushNode::CoverageBrushNode(std::shared_ptr<const CoverageNode> input,const BrushMaskSettings& settings)
    :input_(std::move(input)) {
    if (!input_) throw std::invalid_argument("brush mask input is null");
    extent(input_->native_bounds());validate_brush_mask_settings(settings);
    settings_=settings;
}

Rect CoverageBrushNode::native_bounds() const noexcept {return input_->native_bounds();}
bool CoverageBrushNode::supports_level(RenderLevel level) const noexcept {
    return admitted(level)&&input_->supports_level({0,RenderQuality::Final});
}
RenderLevel CoverageBrushNode::input_level(RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("unsupported brush render level");
    return {0,RenderQuality::Final};
}
Rect CoverageBrushNode::input_region_level(Rect output,Rect input_bounds,RenderLevel level) const {
    if (!same(input_bounds,native_bounds())) throw std::invalid_argument("brush input extent mismatch");
    contained(output_bounds(level),output);
    if (!level.mip) return output;
    const auto native=native_bounds();
    const std::uint64_t scale=1u<<level.mip;
    const auto x=static_cast<std::uint64_t>(output.x)*scale,y=static_cast<std::uint64_t>(output.y)*scale;
    const auto end_x=std::min((static_cast<std::uint64_t>(output.x)+output.width)*scale,static_cast<std::uint64_t>(native.width));
    const auto end_y=std::min((static_cast<std::uint64_t>(output.y)+output.height)*scale,static_cast<std::uint64_t>(native.height));
    return {static_cast<std::uint32_t>(native.x+x),static_cast<std::uint32_t>(native.y+y),
            static_cast<std::uint32_t>(end_x-x),static_cast<std::uint32_t>(end_y-y)};
}

CoverageTile CoverageBrushNode::render_level(Rect bounds,RenderLevel level) const {
    const auto request=input_region_level(bounds,native_bounds(),level);
    (void)count(request);(void)count(bounds);
    auto native=input_->render_level(request,input_level(level));
    if (!same(native.bounds,request)) throw std::domain_error("brush input returned wrong ROI");
    validate_coverage_tile(native);
    for (std::uint32_t y=0;y<request.height;++y) {
        const double py=static_cast<double>(request.y)+y+0.5;
        for (std::uint32_t x=0;x<request.width;++x) {
            const auto i=static_cast<std::size_t>(y)*request.width+x;
            const double px=static_cast<double>(request.x)+x+0.5;
            native.coverage[i]=replay(native.coverage[i],px,py,settings_);
        }
    }
    if (!level.mip) return native;
    CoverageTile output{bounds,std::vector<float>(count(bounds))};
    const auto scale=1u<<level.mip;
    for (std::uint32_t y=0;y<bounds.height;++y) {
        const auto start_y=static_cast<std::uint64_t>(y)*scale;
        const auto end_y=std::min(start_y+scale,static_cast<std::uint64_t>(request.height));
        for (std::uint32_t x=0;x<bounds.width;++x) {
            const auto start_x=static_cast<std::uint64_t>(x)*scale;
            const auto end_x=std::min(start_x+scale,static_cast<std::uint64_t>(request.width));
            double sum=0;
            for (auto v=start_y;v<end_y;++v)
                for (auto u=start_x;u<end_x;++u)
                    sum+=static_cast<double>(native.coverage[static_cast<std::size_t>(v)*request.width+u]);
            const double samples=static_cast<double>((end_x-start_x)*(end_y-start_y));
            output.coverage[static_cast<std::size_t>(y)*bounds.width+x]=static_cast<float>(sum/samples);
        }
    }
    return output;
}

} // namespace rawengine
