#include "GeometryOps.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {
bool same_rect(Rect a, Rect b) {
    return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
}
void addressable(Rect r) {
    if (std::uint64_t(r.x)+r.width>(std::uint64_t{1}<<32) ||
        std::uint64_t(r.y)+r.height>(std::uint64_t{1}<<32))
        throw std::invalid_argument("rotation rectangle exceeds uint32 coordinates");
}
void contained(Rect r, Rect image) {
    addressable(r);
    if (r.x<image.x || r.y<image.y ||
        std::uint64_t(r.x)+r.width>std::uint64_t(image.x)+image.width ||
        std::uint64_t(r.y)+r.height>std::uint64_t(image.y)+image.height)
        throw std::out_of_range("rotation output rectangle is outside image");
}
std::size_t elements(Rect r) {
    if (r.width && r.height>std::vector<float>().max_size()/r.width/3)
        throw std::length_error("rotation output exceeds addressable storage");
    return std::size_t(r.width)*r.height*3;
}
double blend(double a, double b, double t) {
    if (t==0) return a;
    if (t==1) return b;
    if (a==b) return a;
    return a+t*(b-a);
}
float finite_float(double x) {
    if (!std::isfinite(x) || std::abs(x)>std::numeric_limits<float>::max())
        throw std::invalid_argument("rotation output exceeds finite float32 range");
    return static_cast<float>(x);
}
} // namespace

void validate_rotate_settings(const RotateSettings& settings) {
    if (!std::isfinite(settings.angle_degrees) || settings.angle_degrees<-180 || settings.angle_degrees>180)
        throw std::invalid_argument("rotation angle_degrees must be finite in [-180,180]");
}
RotateNode::RotateNode(std::shared_ptr<const Node> input, Rect bounds, RotateSettings settings)
    : input_(std::move(input)), input_bounds_(bounds), settings_(settings) {
    addressable(bounds);validate_rotate_settings(settings_);
    if (!input_ || !bounds.width || !bounds.height)
        throw std::invalid_argument("rotation requires input and nonempty image bounds");
    descriptor_=input_->output_descriptor();
    if (descriptor_!=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        descriptor_!=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("rotation requires scene-linear working RGB");
    cx_=(double(bounds.width)-1)/2;cy_=(double(bounds.height)-1)/2;
    const auto a=settings_.angle_degrees;
    if (a==0) {cosine_=1;sine_=0;}
    else if (a==90) {cosine_=0;sine_=1;}
    else if (a==-90) {cosine_=0;sine_=-1;}
    else if (std::abs(a)==180) {cosine_=-1;sine_=0;}
    else {
        const double theta=(a/180)*0x1.921fb54442d18p+1;
        cosine_=std::cos(theta);sine_=std::sin(theta);
        if (!std::isfinite(cosine_) || !std::isfinite(sine_) || std::abs(cosine_)>1 || std::abs(sine_)>1)
            throw std::invalid_argument("rotation math runtime returned invalid coefficients");
    }
}
RotateNode::Taps RotateNode::taps(std::uint32_t x, std::uint32_t y) const {
    const double dx=double(x)-cx_,dy=double(y)-cy_;
    const double sx=std::clamp((cx_+cosine_*dx)+sine_*dy,0.,double(input_bounds_.width)-1);
    const double sy=std::clamp((cy_-sine_*dx)+cosine_*dy,0.,double(input_bounds_.height)-1);
    const auto ix=static_cast<std::uint32_t>(std::floor(sx)),iy=static_cast<std::uint32_t>(std::floor(sy));
    const double fx=sx-ix,fy=sy-iy;
    return {ix,iy,fx==0 ? ix : std::min(ix+1,input_bounds_.width-1),
                  fy==0 ? iy : std::min(iy+1,input_bounds_.height-1),fx,fy};
}
bool RotateNode::supports_level(RenderLevel level) const noexcept {
    if (level.mip==0 && level.quality==RenderQuality::Final) return input_->supports_level(level);
    return level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview &&
           input_->supports_level({}) && input_->supports_level(level);
}
RenderLevel RotateNode::input_level(RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("rotation has no mapping for render level");
    return {};
}
Rect RotateNode::native_output_region(Rect output, RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("rotation does not support render level");
    const auto scale=1u<<level.mip;
    const Rect extent{0,0,input_bounds_.width/scale+(input_bounds_.width%scale!=0),
                          input_bounds_.height/scale+(input_bounds_.height%scale!=0)};
    contained(output,extent);
    const auto left=std::min(std::uint64_t(output.x)*scale,std::uint64_t(input_bounds_.width));
    const auto top=std::min(std::uint64_t(output.y)*scale,std::uint64_t(input_bounds_.height));
    const auto right=std::min((std::uint64_t(output.x)+output.width)*scale,std::uint64_t(input_bounds_.width));
    const auto bottom=std::min((std::uint64_t(output.y)+output.height)*scale,std::uint64_t(input_bounds_.height));
    return {static_cast<std::uint32_t>(left),static_cast<std::uint32_t>(top),
            static_cast<std::uint32_t>(right-left),static_cast<std::uint32_t>(bottom-top)};
}
Rect RotateNode::support(Rect output) const {
    if (!output.width || !output.height) return {input_bounds_.x,input_bounds_.y,0,0};
    std::uint32_t left=input_bounds_.width,top=input_bounds_.height,right=0,bottom=0;
    // PERF-030: scan actual floating taps, not unproved rounded corner extrema.
    // Rendering repeats map work to keep coordinate meshes out of bounded scratch.
    for (std::uint64_t y=output.y;y<std::uint64_t(output.y)+output.height;++y)
        for (std::uint64_t x=output.x;x<std::uint64_t(output.x)+output.width;++x) {
            const auto p=taps(static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y));
            left=std::min(left,p.x0);top=std::min(top,p.y0);right=std::max(right,p.x1);bottom=std::max(bottom,p.y1);
        }
    return {static_cast<std::uint32_t>(std::uint64_t(input_bounds_.x)+left),
            static_cast<std::uint32_t>(std::uint64_t(input_bounds_.y)+top),right-left+1,bottom-top+1};
}
Rect RotateNode::input_region(Rect output, Rect bounds) const {
    return input_region_level(output,bounds,{});
}
Rect RotateNode::input_region_level(Rect output, Rect bounds, RenderLevel level) const {
    if (!same_rect(bounds,input_bounds_)) throw std::invalid_argument("rotation input bounds differ from construction");
    return support(native_output_region(output,level));
}
Tile RotateNode::native_block(Rect output) const {
    const auto mapped=support(output);
    if (output.width>128 || output.height>128 || mapped.width>257 || mapped.height>257)
        throw std::length_error("rotation block exceeds bounded source or scratch extent");
    auto source=input_->render(mapped);
    if (!same_rect(source.bounds,mapped) || source.descriptor!=descriptor_ || source.rgb.size()!=elements(mapped))
        throw std::invalid_argument("rotation input bounds, storage or descriptor differ");
    for (float value:source.rgb) if (!std::isfinite(value))
        throw std::invalid_argument("rotation requires finite complete input rectangle");
    Tile result{output,std::vector<float>(elements(output)),descriptor_};
    for (std::uint32_t y=0;y<output.height;++y) for (std::uint32_t x=0;x<output.width;++x) {
        const auto p=taps(output.x+x,output.y+y);
        const auto value=[&](std::uint32_t xx,std::uint32_t yy,unsigned channel) {
            const auto sx=std::uint64_t(input_bounds_.x)+xx-mapped.x;
            const auto sy=std::uint64_t(input_bounds_.y)+yy-mapped.y;
            return double(source.rgb[(std::size_t(sy)*mapped.width+std::size_t(sx))*3+channel]);
        };
        for (unsigned c=0;c<3;++c) {
            const double upper=blend(value(p.x0,p.y0,c),value(p.x1,p.y0,c),p.fx);
            const double lower=blend(value(p.x0,p.y1,c),value(p.x1,p.y1,c),p.fx);
            result.rgb[(std::size_t(y)*output.width+x)*3+c]=finite_float(blend(upper,lower,p.fy));
        }
    }
    return result;
}
Tile RotateNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile RotateNode::render_level(Rect bounds, RenderLevel level) const {
    (void)native_output_region(bounds,level);
    Tile result{bounds,std::vector<float>(elements(bounds)),descriptor_};
    if (!bounds.width || !bounds.height) return result;
    const auto scale=1u<<level.mip,block_size=128u/scale;
    for (std::uint64_t by=bounds.y;by<std::uint64_t(bounds.y)+bounds.height;by+=block_size)
        for (std::uint64_t bx=bounds.x;bx<std::uint64_t(bounds.x)+bounds.width;bx+=block_size) {
            const Rect block{static_cast<std::uint32_t>(bx),static_cast<std::uint32_t>(by),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size,std::uint64_t(bounds.x)+bounds.width-bx)),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size,std::uint64_t(bounds.y)+bounds.height-by))};
            const auto native=native_block(native_output_region(block,level));
            for (std::uint32_t y=0;y<block.height;++y) for (std::uint32_t x=0;x<block.width;++x) {
                const auto target=(std::size_t(by+y-bounds.y)*bounds.width+std::size_t(bx+x-bounds.x))*3;
                if (!level.mip) {
                    const auto source=(std::size_t(y)*native.bounds.width+x)*3;
                    std::copy_n(native.rgb.begin()+source,3,result.rgb.begin()+target);
                } else {
                    double sums[3]{};unsigned count=0;
                    for (std::uint32_t iy=y*scale;iy<std::min((y+1)*scale,native.bounds.height);++iy)
                        for (std::uint32_t ix=x*scale;ix<std::min((x+1)*scale,native.bounds.width);++ix) {
                            const auto source=(std::size_t(iy)*native.bounds.width+ix)*3;
                            for (unsigned c=0;c<3;++c) sums[c]=sums[c]+double(native.rgb[source+c]);
                            ++count;
                        }
                    for (unsigned c=0;c<3;++c) result.rgb[target+c]=finite_float(sums[c]/count);
                }
            }
        }
    return result;
}
void validate_projective_settings(const ProjectiveSettings& settings) {
    if (!settings.width || !settings.height)
        throw std::invalid_argument("projective requires positive canvas width and height");
    const auto& m=settings.source_from_output;
    for (double value:m) if (!std::isfinite(value) || value<-16 || value>16)
        throw std::invalid_argument("projective coefficients must be finite in [-16,16]");
    if (m[8]!=1) throw std::invalid_argument("projective m22 must equal one");
    const double determinant=(m[0]*(m[4]-m[5]*m[7])-m[1]*(m[3]-m[5]*m[6]))+
                             m[2]*(m[3]*m[7]-m[4]*m[6]);
    if (std::abs(determinant)<0x1p-20)
        throw std::invalid_argument("projective determinant is below admission threshold");
    for (double u:{-1.,1.}) for (double v:{-1.,1.})
        if ((m[6]*u+m[7]*v)+1<.25)
            throw std::invalid_argument("projective denominator corner is below margin");
}
ProjectiveNode::ProjectiveNode(std::shared_ptr<const Node> input,Rect bounds,ProjectiveSettings settings)
    : input_(std::move(input)),input_bounds_(bounds),settings_(settings),identity_(false) {
    addressable(bounds);validate_projective_settings(settings_);
    if (!input_ || !bounds.width || !bounds.height)
        throw std::invalid_argument("projective requires input and nonempty image bounds");
    descriptor_=input_->output_descriptor();
    if (descriptor_!=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        descriptor_!=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("projective requires scene-linear working RGB");
    identity_=settings_.source_from_output==std::array<double,9>{1,0,0,0,1,0,0,0,1} &&
              settings_.width==bounds.width && settings_.height==bounds.height;
}
ProjectiveNode::Taps ProjectiveNode::taps(std::uint32_t x,std::uint32_t y) const {
    if (identity_) return {x,y,x,y,0,0};
    const auto& m=settings_.source_from_output;
    const double u=settings_.width==1 ? 0 : (2*double(x)-double(settings_.width-1))/double(settings_.width-1);
    const double v=settings_.height==1 ? 0 : (2*double(y)-double(settings_.height-1))/double(settings_.height-1);
    const double denominator=(m[6]*u+m[7]*v)+1;
    if (!std::isfinite(denominator) || denominator<.25)
        throw std::invalid_argument("projective sample denominator is below margin");
    const double nx=((m[0]*u+m[1]*v)+m[2])/denominator;
    const double ny=((m[3]*u+m[4]*v)+m[5])/denominator;
    const double sx=std::clamp(((nx+1)*.5)*double(input_bounds_.width-1),0.,double(input_bounds_.width-1));
    const double sy=std::clamp(((ny+1)*.5)*double(input_bounds_.height-1),0.,double(input_bounds_.height-1));
    const auto ix=static_cast<std::uint32_t>(std::floor(sx)),iy=static_cast<std::uint32_t>(std::floor(sy));
    const double fx=sx-ix,fy=sy-iy;
    return {ix,iy,fx==0 ? ix : std::min(ix+1,input_bounds_.width-1),
                  fy==0 ? iy : std::min(iy+1,input_bounds_.height-1),fx,fy};
}
bool ProjectiveNode::supports_level(RenderLevel level) const noexcept {
    if (level.mip==0 && level.quality==RenderQuality::Final) return input_->supports_level(level);
    return level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview &&
           input_->supports_level({}) && input_->supports_level(level);
}
RenderLevel ProjectiveNode::input_level(RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("projective has no mapping for render level");
    return {};
}
Rect ProjectiveNode::native_output_region(Rect output,RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("projective does not support render level");
    const auto scale=1u<<level.mip;
    contained(output,{0,0,settings_.width/scale+(settings_.width%scale!=0),
                          settings_.height/scale+(settings_.height%scale!=0)});
    const auto left=std::min(std::uint64_t(output.x)*scale,std::uint64_t(settings_.width));
    const auto top=std::min(std::uint64_t(output.y)*scale,std::uint64_t(settings_.height));
    const auto right=std::min((std::uint64_t(output.x)+output.width)*scale,std::uint64_t(settings_.width));
    const auto bottom=std::min((std::uint64_t(output.y)+output.height)*scale,std::uint64_t(settings_.height));
    return {static_cast<std::uint32_t>(left),static_cast<std::uint32_t>(top),
            static_cast<std::uint32_t>(right-left),static_cast<std::uint32_t>(bottom-top)};
}
Rect ProjectiveNode::support(Rect output) const {
    if (!output.width || !output.height) return {input_bounds_.x,input_bounds_.y,0,0};
    std::uint32_t left=input_bounds_.width,top=input_bounds_.height,right=0,bottom=0;
    // PERF-031: share actual floating taps; repeated scans trade work for bounded scratch.
    for (std::uint64_t y=output.y;y<std::uint64_t(output.y)+output.height;++y)
        for (std::uint64_t x=output.x;x<std::uint64_t(output.x)+output.width;++x) {
            const auto p=taps(static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y));
            left=std::min(left,p.x0);top=std::min(top,p.y0);right=std::max(right,p.x1);bottom=std::max(bottom,p.y1);
        }
    return {static_cast<std::uint32_t>(std::uint64_t(input_bounds_.x)+left),
            static_cast<std::uint32_t>(std::uint64_t(input_bounds_.y)+top),right-left+1,bottom-top+1};
}
Rect ProjectiveNode::input_region(Rect output,Rect bounds) const {return input_region_level(output,bounds,{});}
Rect ProjectiveNode::input_region_level(Rect output,Rect bounds,RenderLevel level) const {
    if (!same_rect(bounds,input_bounds_)) throw std::invalid_argument("projective input bounds differ from construction");
    return support(native_output_region(output,level));
}
void ProjectiveNode::sample_into(Rect output,Rect mapped,Tile& native) const {
    if (mapped.width>257 || mapped.height>257 || native.bounds.width>128 || native.bounds.height>128)
        throw std::length_error("projective source or native scratch exceeds bounded extent");
    auto source=input_->render(mapped);
    if (!same_rect(source.bounds,mapped) || source.descriptor!=descriptor_ || source.rgb.size()!=elements(mapped))
        throw std::invalid_argument("projective input bounds, storage or descriptor differ");
    for (float value:source.rgb) if (!std::isfinite(value))
        throw std::invalid_argument("projective requires finite complete input rectangle");
    for (std::uint32_t y=0;y<output.height;++y) for (std::uint32_t x=0;x<output.width;++x) {
        const auto p=taps(output.x+x,output.y+y);
        const auto value=[&](std::uint32_t xx,std::uint32_t yy,unsigned channel) {
            const auto sx=std::uint64_t(input_bounds_.x)+xx-mapped.x;
            const auto sy=std::uint64_t(input_bounds_.y)+yy-mapped.y;
            return double(source.rgb[(std::size_t(sy)*mapped.width+std::size_t(sx))*3+channel]);
        };
        const auto target=(std::size_t(output.y+y-native.bounds.y)*native.bounds.width+output.x+x-native.bounds.x)*3;
        for (unsigned c=0;c<3;++c) {
            const double top=blend(value(p.x0,p.y0,c),value(p.x1,p.y0,c),p.fx);
            const double bottom=blend(value(p.x0,p.y1,c),value(p.x1,p.y1,c),p.fx);
            native.rgb[target+c]=finite_float(blend(top,bottom,p.fy));
        }
    }
}
void ProjectiveNode::fallback_row(Rect row,Tile& native) const {
    const auto mapped=support(row);
    if (mapped.width<=257 && mapped.height<=257) {sample_into(row,mapped,native);return;}
    if (row.width<=1) throw std::length_error("projective single sample support exceeds cap");
    const auto first=row.width/2;
    fallback_row({row.x,row.y,first,1},native);
    fallback_row({row.x+first,row.y,row.width-first,1},native);
}
Tile ProjectiveNode::native_block(Rect output) const {
    if (output.width>128 || output.height>128)
        throw std::length_error("projective native scratch exceeds bounded extent");
    Tile native{output,std::vector<float>(elements(output)),descriptor_};
    const auto mapped=support(output);
    if (mapped.width<=257 && mapped.height<=257) sample_into(output,mapped,native);
    else {
        if (output.width>4 || output.height>4) throw std::length_error("projective fallback requires one preview cell");
        for (std::uint32_t y=0;y<output.height;++y) fallback_row({output.x,output.y+y,output.width,1},native);
    }
    return native;
}
void ProjectiveNode::render_block(Rect block,RenderLevel level,Tile& result) const {
    const auto region=native_output_region(block,level),mapped=support(region);
    if ((mapped.width>257 || mapped.height>257) && (block.width>1 || block.height>1)) {
        if (block.width>=block.height && block.width>1) {
            const auto first=block.width/2;
            render_block({block.x,block.y,first,block.height},level,result);
            render_block({block.x+first,block.y,block.width-first,block.height},level,result);
        } else {
            const auto first=block.height/2;
            render_block({block.x,block.y,block.width,first},level,result);
            render_block({block.x,block.y+first,block.width,block.height-first},level,result);
        }
        return;
    }
    const auto native=native_block(region); // Source released before reduction.
    const auto scale=1u<<level.mip;
    for (std::uint32_t y=0;y<block.height;++y) for (std::uint32_t x=0;x<block.width;++x) {
        const auto target=(std::size_t(block.y+y-result.bounds.y)*result.bounds.width+block.x+x-result.bounds.x)*3;
        if (!level.mip) {
            const auto source=(std::size_t(y)*native.bounds.width+x)*3;
            std::copy_n(native.rgb.begin()+source,3,result.rgb.begin()+target);
        } else {
            double sums[3]{};unsigned count=0;
            for (std::uint32_t iy=y*scale;iy<std::min((y+1)*scale,native.bounds.height);++iy)
                for (std::uint32_t ix=x*scale;ix<std::min((x+1)*scale,native.bounds.width);++ix) {
                    const auto source=(std::size_t(iy)*native.bounds.width+ix)*3;
                    for (unsigned c=0;c<3;++c) sums[c]=sums[c]+double(native.rgb[source+c]);
                    ++count;
                }
            for (unsigned c=0;c<3;++c) result.rgb[target+c]=finite_float(sums[c]/count);
        }
    }
}
Tile ProjectiveNode::render(Rect bounds) const {return render_level(bounds,{});}
Tile ProjectiveNode::render_level(Rect bounds,RenderLevel level) const {
    (void)native_output_region(bounds,level);
    Tile result{bounds,std::vector<float>(elements(bounds)),descriptor_};
    if (!bounds.width || !bounds.height) return result;
    const auto block_size=128u/(1u<<level.mip);
    for (std::uint64_t y=bounds.y;y<std::uint64_t(bounds.y)+bounds.height;y+=block_size)
        for (std::uint64_t x=bounds.x;x<std::uint64_t(bounds.x)+bounds.width;x+=block_size)
            render_block({static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size,std::uint64_t(bounds.x)+bounds.width-x)),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size,std::uint64_t(bounds.y)+bounds.height-y))},level,result);
    return result;
}
void validate_cubic_resize_settings(const CubicResizeSettings& settings,Rect bounds) {
    addressable(bounds);
    if (!bounds.width || !bounds.height || !settings.width || !settings.height)
        throw std::invalid_argument("cubic resize requires positive input and canvas extents");
    if (std::uint64_t(settings.width)*4<bounds.width || std::uint64_t(settings.height)*4<bounds.height)
        throw std::invalid_argument("cubic resize native shrink exceeds fourfold per axis");
}
CubicResizeNode::CubicResizeNode(std::shared_ptr<const Node> input,Rect bounds,CubicResizeSettings settings)
    : input_(std::move(input)),input_bounds_(bounds),settings_(settings) {
    validate_cubic_resize_settings(settings_,bounds);
    if (!input_) throw std::invalid_argument("cubic resize requires input");
    descriptor_=input_->output_descriptor();
    if (descriptor_!=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        descriptor_!=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("cubic resize requires scene-linear working RGB");
}
CubicResizeNode::Axis CubicResizeNode::axis(std::uint32_t input,std::uint32_t output,std::uint32_t index) {
    Axis result{};
    if (input==output) {result.center=index;result.count=1;result.taps[0]={index,1};return result;}
    const double z=std::clamp(((double(index)+.5)*double(input)/double(output))-.5,0.,double(input-1));
    const double scale=std::max(1.,double(input)/double(output));
    result.center=static_cast<std::uint32_t>(std::floor(z));
    const auto first=static_cast<std::int64_t>(std::floor(z-2*scale));
    const auto last=static_cast<std::int64_t>(std::ceil(z+2*scale));
    double total=0;
    for (auto i=first;i<=last;++i) {
        const double distance=std::abs((double(i)-z)/scale);
        double value=0;
        if (distance<1) value=((1.5*distance-2.5)*distance)*distance+1;
        else if (distance<2) value=(-.5*(distance-1))*((distance-2)*(distance-2));
        const double weight=value/scale;
        if (weight==0) continue;
        if (result.count==result.taps.size()) throw std::invalid_argument("cubic resize axis tap cap exceeded");
        const auto physical=static_cast<std::uint32_t>(std::clamp(i,std::int64_t{0},std::int64_t(input)-1));
        result.taps[result.count++]={physical,weight};total=total+weight;
    }
    if (!std::isfinite(total) || total<=.5) throw std::invalid_argument("cubic resize normalization guard failed");
    for (unsigned i=0;i<result.count;++i) {
        auto& weight=result.taps[i].weight;weight=weight/total;
        if (!std::isfinite(weight) || std::abs(weight)>4)
            throw std::invalid_argument("cubic resize axis weight guard failed");
    }
    return result;
}
bool CubicResizeNode::supports_level(RenderLevel level) const noexcept {
    if (level.mip==0 && level.quality==RenderQuality::Final) return input_->supports_level(level);
    return level.mip>=1 && level.mip<=2 && level.quality==RenderQuality::Preview &&
           input_->supports_level({}) && input_->supports_level(level);
}
RenderLevel CubicResizeNode::input_level(RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("cubic resize has no mapping for render level");
    return {};
}
Rect CubicResizeNode::native_output_region(Rect output,RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("cubic resize does not support render level");
    const auto scale=1u<<level.mip;
    contained(output,{0,0,settings_.width/scale+(settings_.width%scale!=0),
                          settings_.height/scale+(settings_.height%scale!=0)});
    const auto left=std::min(std::uint64_t(output.x)*scale,std::uint64_t(settings_.width));
    const auto top=std::min(std::uint64_t(output.y)*scale,std::uint64_t(settings_.height));
    const auto right=std::min((std::uint64_t(output.x)+output.width)*scale,std::uint64_t(settings_.width));
    const auto bottom=std::min((std::uint64_t(output.y)+output.height)*scale,std::uint64_t(settings_.height));
    return {static_cast<std::uint32_t>(left),static_cast<std::uint32_t>(top),
            static_cast<std::uint32_t>(right-left),static_cast<std::uint32_t>(bottom-top)};
}
Rect CubicResizeNode::support(Rect output) const {
    if (!output.width || !output.height) return {input_bounds_.x,input_bounds_.y,0,0};
    const auto extent=[](std::uint32_t input,std::uint32_t canvas,std::uint32_t start,std::uint32_t length) {
        std::uint32_t first=input,last=0;
        for (std::uint64_t i=start;i<std::uint64_t(start)+length;++i) {
            const auto a=axis(input,canvas,static_cast<std::uint32_t>(i));
            for (unsigned j=0;j<a.count;++j) {first=std::min(first,a.taps[j].index);last=std::max(last,a.taps[j].index);}
        }
        return std::pair{first,last-first+1};
    };
    const auto x=extent(input_bounds_.width,settings_.width,output.x,output.width);
    const auto y=extent(input_bounds_.height,settings_.height,output.y,output.height);
    return {static_cast<std::uint32_t>(std::uint64_t(input_bounds_.x)+x.first),
            static_cast<std::uint32_t>(std::uint64_t(input_bounds_.y)+y.first),x.second,y.second};
}
Rect CubicResizeNode::input_region(Rect output,Rect bounds) const {return input_region_level(output,bounds,{});}
Rect CubicResizeNode::input_region_level(Rect output,Rect bounds,RenderLevel level) const {
    if (!same_rect(bounds,input_bounds_)) throw std::invalid_argument("cubic resize input bounds differ from construction");
    return support(native_output_region(output,level));
}
Tile CubicResizeNode::native_block(Rect output) const {
    const auto mapped=support(output);
    if (output.width>32 || output.height>32 || mapped.width>145 || mapped.height>145)
        throw std::length_error("cubic resize bounded source or native scratch cap exceeded");
    auto source=input_->render(mapped);
    if (!same_rect(source.bounds,mapped) || source.descriptor!=descriptor_ || source.rgb.size()!=elements(mapped))
        throw std::invalid_argument("cubic resize input bounds, storage or descriptor differ");
    for (float value:source.rgb) if (!std::isfinite(value))
        throw std::invalid_argument("cubic resize requires finite complete input rectangle");
    // Identity still validates the complete fetched tile before rebasing it.
    if (settings_.width==input_bounds_.width && settings_.height==input_bounds_.height) {
        source.bounds=output;
        return source;
    }
    std::array<Axis,32> xs{},ys{};
    for (unsigned x=0;x<output.width;++x) xs[x]=axis(input_bounds_.width,settings_.width,output.x+x);
    for (unsigned y=0;y<output.height;++y) ys[y]=axis(input_bounds_.height,settings_.height,output.y+y);
    Tile result{output,std::vector<float>(elements(output)),descriptor_};
    // PERF-032: reuse center/consecutive replicated rows with scalar storage;
    // retain every logical vertical tap and its original addition order.
    for (unsigned y=0;y<output.height;++y) for (unsigned x=0;x<output.width;++x) {
        const auto& ax=xs[x];const auto& ay=ys[y];
        const auto horizontal=[&](std::uint32_t row,unsigned c) {
            const auto value=[&](std::uint32_t column) {
                const auto sx=std::uint64_t(input_bounds_.x)+column-mapped.x;
                const auto sy=std::uint64_t(input_bounds_.y)+row-mapped.y;
                return double(source.rgb[(std::size_t(sy)*mapped.width+std::size_t(sx))*3+c]);
            };
            const double base=value(ax.center);double v=base;
            for (unsigned i=0;i<ax.count;++i) {
                const double delta=ax.taps[i].weight*(value(ax.taps[i].index)-base);
                if (delta!=0) v=v+delta;
            }
            return v;
        };
        for (unsigned c=0;c<3;++c) {
            const double base=horizontal(ay.center,c);double v=base;
            std::uint32_t previous_row=ay.center;
            double previous_value=base;
            for (unsigned i=0;i<ay.count;++i) {
                const auto row=ay.taps[i].index;
                const double row_value=row==ay.center?base:
                    (row==previous_row?previous_value:horizontal(row,c));
                previous_row=row;previous_value=row_value;
                const double delta=ay.taps[i].weight*(row_value-base);
                if (delta!=0) v=v+delta;
            }
            result.rgb[(std::size_t(y)*output.width+x)*3+c]=finite_float(v);
        }
    }
    return result;
}
Tile CubicResizeNode::render(Rect bounds) const { return render_level(bounds,{}); }
Tile CubicResizeNode::render_level(Rect bounds, RenderLevel level) const {
    (void)native_output_region(bounds,level);
    Tile result{bounds,std::vector<float>(elements(bounds)),descriptor_};
    if (!bounds.width || !bounds.height) return result;
    const auto scale=1u<<level.mip,block_size=32u/scale;
    for (std::uint64_t by=bounds.y;by<std::uint64_t(bounds.y)+bounds.height;by+=block_size)
        for (std::uint64_t bx=bounds.x;bx<std::uint64_t(bounds.x)+bounds.width;bx+=block_size) {
            const Rect block{static_cast<std::uint32_t>(bx),static_cast<std::uint32_t>(by),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size,std::uint64_t(bounds.x)+bounds.width-bx)),
                static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size,std::uint64_t(bounds.y)+bounds.height-by))};
            const auto native=native_block(native_output_region(block,level));
            for (std::uint32_t y=0;y<block.height;++y) for (std::uint32_t x=0;x<block.width;++x) {
                const auto target=(std::size_t(by+y-bounds.y)*bounds.width+std::size_t(bx+x-bounds.x))*3;
                if (!level.mip) {
                    const auto source=(std::size_t(y)*native.bounds.width+x)*3;
                    std::copy_n(native.rgb.begin()+source,3,result.rgb.begin()+target);
                } else {
                    double sums[3]{};unsigned count=0;
                    for (std::uint32_t iy=y*scale;iy<std::min((y+1)*scale,native.bounds.height);++iy)
                        for (std::uint32_t ix=x*scale;ix<std::min((x+1)*scale,native.bounds.width);++ix) {
                            const auto source=(std::size_t(iy)*native.bounds.width+ix)*3;
                            for (unsigned c=0;c<3;++c) sums[c]=sums[c]+double(native.rgb[source+c]);
                            ++count;
                        }
                    for (unsigned c=0;c<3;++c) result.rgb[target+c]=finite_float(sums[c]/count);
                }
            }
        }
    return result;
}
} // namespace rawengine
