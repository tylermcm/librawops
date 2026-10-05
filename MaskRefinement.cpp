#include "MaskRefinement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}
void extent(Rect r) {
    const auto maximum=std::numeric_limits<std::uint32_t>::max();
    if (!r.width || !r.height || r.x>maximum-r.width || r.y>maximum-r.height)
        throw std::invalid_argument("mask refinement extent is empty or overflows");
}
void input(const std::shared_ptr<const CoverageNode>& mask) {
    if (!mask) throw std::invalid_argument("mask refinement input is null");
    extent(mask->native_bounds());
}
bool admitted(RenderLevel l) {
    return (!l.mip && (l.quality==RenderQuality::Final || l.quality==RenderQuality::Preview)) ||
           ((l.mip==1 || l.mip==2) && l.quality==RenderQuality::Preview);
}
RenderLevel native_level(const CoverageNode& node,RenderLevel l) {
    if (!node.supports_level(l)) throw std::invalid_argument("unsupported mask refinement level");
    return {};
}
Rect block(const CoverageNode& node,Rect output,RenderLevel level) {
    (void)native_level(node,level);const auto bounds=node.output_bounds(level);
    if (!output.width || !output.height || output.x<bounds.x || output.y<bounds.y ||
        output.width>bounds.width || output.height>bounds.height || output.x-bounds.x>bounds.width-output.width ||
        output.y-bounds.y>bounds.height-output.height) throw std::out_of_range("mask refinement ROI is outside extent");
    if (!level.mip) return output;
    const auto native=node.native_bounds();const std::uint64_t scale=1u<<level.mip;
    const auto x=static_cast<std::uint64_t>(output.x)*scale,y=static_cast<std::uint64_t>(output.y)*scale;
    const auto right=std::min((static_cast<std::uint64_t>(output.x)+output.width)*scale,static_cast<std::uint64_t>(native.width));
    const auto bottom=std::min((static_cast<std::uint64_t>(output.y)+output.height)*scale,static_cast<std::uint64_t>(native.height));
    return {static_cast<std::uint32_t>(native.x+x),static_cast<std::uint32_t>(native.y+y),
            static_cast<std::uint32_t>(right-x),static_cast<std::uint32_t>(bottom-y)};
}
Rect expand(Rect r,Rect canvas,std::uint32_t rx,std::uint32_t ry) {
    const std::uint64_t left=std::max<std::uint64_t>(canvas.x,r.x>=rx?r.x-rx:0);
    const std::uint64_t top=std::max<std::uint64_t>(canvas.y,r.y>=ry?r.y-ry:0);
    const auto right=std::min(static_cast<std::uint64_t>(r.x)+r.width+rx,static_cast<std::uint64_t>(canvas.x)+canvas.width);
    const auto bottom=std::min(static_cast<std::uint64_t>(r.y)+r.height+ry,static_cast<std::uint64_t>(canvas.y)+canvas.height);
    return {static_cast<std::uint32_t>(left),static_cast<std::uint32_t>(top),
            static_cast<std::uint32_t>(right-left),static_cast<std::uint32_t>(bottom-top)};
}
Rect region(const CoverageNode& node,Rect output,Rect input_bounds,RenderLevel l,std::uint32_t halo) {
    if (!same(input_bounds,node.native_bounds())) throw std::invalid_argument("mask refinement input extent mismatch");
    return expand(block(node,output,l),input_bounds,halo,halo);
}
template<class T> std::size_t count(Rect r,std::size_t channels=1) {
    const auto n=static_cast<std::uint64_t>(r.width)*r.height;
    if (n>std::numeric_limits<std::size_t>::max()/sizeof(T)/channels || n>std::vector<T>{}.max_size()/channels)
        throw std::invalid_argument("mask refinement storage capacity overflows");
    return static_cast<std::size_t>(n)*channels;
}
void payload(std::initializer_list<std::size_t> bytes) {
    std::size_t total=0;
    for (auto b:bytes) {
        if (b>std::numeric_limits<std::size_t>::max()-total) throw std::invalid_argument("mask refinement total payload overflows");
        total+=b;
    }
}
std::size_t index(Rect r,std::uint64_t x,std::uint64_t y) {
    return static_cast<std::size_t>(y-r.y)*r.width+static_cast<std::size_t>(x-r.x);
}
void coverage(const CoverageTile& tile,Rect request) {
    if (!same(tile.bounds,request)) throw std::domain_error("mask refinement scalar returned wrong ROI");
    validate_coverage_tile(tile);
}
std::array<double,2> guide_weights(ImageDescriptor d) {
    if (d==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50)) return {0.28807112822929337,0.00008565396060525903};
    if (d==ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65)) return {0.26270021201126703,0.059301716469861945};
    throw std::invalid_argument("mask refinement guide requires scene-linear ProPhoto/D50 or Rec.2020/D65 RGB");
}
void guide(const Tile& tile,Rect request,ImageDescriptor d) {
    if (!same(tile.bounds,request) || tile.descriptor!=d || tile.rgb.size()!=count<float>(request,3))
        throw std::domain_error("mask refinement RGB guide returned wrong ROI, descriptor or storage");
    for (float v:tile.rgb) if (!std::isfinite(v)) throw std::domain_error("mask refinement guide is nonfinite");
}
double finite(double v) {
    if (!std::isfinite(v)) throw std::domain_error("mask refinement numeric stage is nonfinite");
    return v;
}
float stored(double v) {
    v=std::clamp(finite(v),0.0,1.0);
    const float f=static_cast<float>(v);
    return f==0?0.0f:f;
}
float density(float a,double d) {
    if (d==1) return a;
    if (d==0) return 1;
    const double u=1-static_cast<double>(a),v=d*u,q=1-v;
    return stored(q);
}
CoverageTile reduce(CoverageTile native,Rect output,RenderLevel l) {
    if (!l.mip) return native;
    CoverageTile result{output,std::vector<float>(count<float>(output))};const auto scale=1u<<l.mip;
    for (std::uint32_t y=0;y<output.height;++y) for (std::uint32_t x=0;x<output.width;++x) {
        const auto sx=static_cast<std::uint64_t>(x)*scale,sy=static_cast<std::uint64_t>(y)*scale;
        const auto ex=std::min(sx+scale,static_cast<std::uint64_t>(native.bounds.width));
        const auto ey=std::min(sy+scale,static_cast<std::uint64_t>(native.bounds.height));
        double sum=0;
        for (auto v=sy;v<ey;++v) for (auto u=sx;u<ex;++u)
            sum+=static_cast<double>(native.coverage[static_cast<std::size_t>(v)*native.bounds.width+u]);
        result.coverage[static_cast<std::size_t>(y)*output.width+x]=stored(sum/static_cast<double>((ex-sx)*(ey-sy)));
    }
    return result;
}
template<class F> void window(std::uint64_t x,std::uint64_t y,std::uint32_t radius,Rect canvas,F f) {
    const auto left=std::max<std::uint64_t>(canvas.x,x>=radius?x-radius:0);
    const auto top=std::max<std::uint64_t>(canvas.y,y>=radius?y-radius:0);
    const auto right=std::min(x+radius+1,static_cast<std::uint64_t>(canvas.x)+canvas.width);
    const auto bottom=std::min(y+radius+1,static_cast<std::uint64_t>(canvas.y)+canvas.height);
    for (auto yy=top;yy<bottom;++yy) for (auto xx=left;xx<right;++xx) f(xx,yy);
}
double difference(const Tile& g,std::size_t j,std::size_t k,std::array<double,2> w) {
    const double dr=static_cast<double>(g.rgb[3*j])-static_cast<double>(g.rgb[3*k]);
    const double dg=static_cast<double>(g.rgb[3*j+1])-static_cast<double>(g.rgb[3*k+1]);
    const double db=static_cast<double>(g.rgb[3*j+2])-static_cast<double>(g.rgb[3*k+2]);
    const double ur=dr-dg,ub=db-dg,pr=w[0]*ur,pb=w[1]*ub,s=dg+pr,d=s+pb;
    return finite(d);
}
} // namespace

void validate_mask_density_settings(const MaskDensitySettings& s) {
    if (!std::isfinite(s.density) || s.density<0 || s.density>1) throw std::invalid_argument("mask density must be finite in [0,1]");
}
void validate_mask_feather_settings(const MaskFeatherSettings& s) {
    if (s.radius>32) throw std::invalid_argument("mask feather radius must be in [0,32]");
}
void validate_mask_refine_settings(const MaskRefineSettings& s) {
    if (s.radius>8 || !std::isfinite(s.epsilon) || s.epsilon<0x1p-24 || s.epsilon>65536)
        throw std::invalid_argument("mask refinement radius/epsilon exceeds finite bounds");
}
float evaluate_mask_density(float value,const MaskDensitySettings& s) {
    validate_mask_density_settings(s);
    if (!std::isfinite(value) || value<0 || value>1) throw std::domain_error("mask density input must be finite in [0,1]");
    return density(value,s.density);
}
CoverageDensityNode::CoverageDensityNode(std::shared_ptr<const CoverageNode> mask,const MaskDensitySettings& s)
    :mask_(std::move(mask)),settings_(s) {input(mask_);validate_mask_density_settings(settings_);}
bool CoverageDensityNode::supports_level(RenderLevel l) const noexcept {return admitted(l)&&mask_->supports_level({});}
Rect CoverageDensityNode::native_bounds() const noexcept {return mask_->native_bounds();}
RenderLevel CoverageDensityNode::input_level(RenderLevel l) const {return native_level(*this,l);}
Rect CoverageDensityNode::input_region_level(Rect o,Rect i,RenderLevel l) const {return region(*this,o,i,l,0);}
CoverageTile CoverageDensityNode::render_level(Rect o,RenderLevel l) const {
    const auto request=block(*this,o,l);const auto n=count<float>(request),p=l.mip?count<float>(o):0;
    payload({n*sizeof(float),p*sizeof(float)});
    auto data=mask_->render_level(request,{});coverage(data,request);
    for (auto& a:data.coverage) a=density(a,settings_.density);
    return reduce(std::move(data),o,l);
}
CoverageFeatherNode::CoverageFeatherNode(std::shared_ptr<const CoverageNode> mask,const MaskFeatherSettings& s)
    :mask_(std::move(mask)),settings_(s) {
    input(mask_);validate_mask_feather_settings(settings_);
    std::array<std::uint64_t,65> row{};row[0]=1;
    for (unsigned n=1;n<=2*settings_.radius;++n) for (unsigned k=n;k;--k) row[k]+=row[k-1];
    for (unsigned k=0;k<=2*settings_.radius;++k) weights_[k]=static_cast<double>(row[k]);
}
bool CoverageFeatherNode::supports_level(RenderLevel l) const noexcept {return admitted(l)&&mask_->supports_level({});}
Rect CoverageFeatherNode::native_bounds() const noexcept {return mask_->native_bounds();}
RenderLevel CoverageFeatherNode::input_level(RenderLevel l) const {return native_level(*this,l);}
Rect CoverageFeatherNode::input_region_level(Rect o,Rect i,RenderLevel l) const {return region(*this,o,i,l,settings_.radius);}
CoverageTile CoverageFeatherNode::render_level(Rect o,RenderLevel l) const {
    const auto output=block(*this,o,l),canvas=native_bounds();const auto r=settings_.radius;
    const auto centers=expand(output,canvas,0,r),request=expand(centers,canvas,r,0);
    const auto n=count<float>(request),a=r?count<double>(centers):0,b=r?count<float>(output):0,p=l.mip?count<float>(o):0;
    payload({n*sizeof(float),a*sizeof(double),b*sizeof(float),p*sizeof(float)});
    auto data=mask_->render_level(request,{});coverage(data,request);
    if (!r) return reduce(std::move(data),o,l);
    std::vector<double> horizontal(a);
    for (std::uint64_t y=centers.y;y<static_cast<std::uint64_t>(centers.y)+centers.height;++y)
        for (std::uint64_t x=centers.x;x<static_cast<std::uint64_t>(centers.x)+centers.width;++x) {
            const auto left=std::max<std::uint64_t>(canvas.x,x>=r?x-r:0);
            const auto right=std::min(x+r+1,static_cast<std::uint64_t>(canvas.x)+canvas.width);
            const double center=data.coverage[index(request,x,y)];bool equal=true;double sum=0,mass=0;
            for (auto xx=left;xx<right;++xx) {
                const double v=data.coverage[index(request,xx,y)];equal=equal&&(v==center);
                const double w=weights_[static_cast<std::size_t>(static_cast<std::int64_t>(xx)-static_cast<std::int64_t>(x)+r)];
                const double product=w*v;sum+=product;mass+=w;
            }
            horizontal[index(centers,x,y)]=equal?center:finite(sum/mass);
        }
    CoverageTile mapped{output,std::vector<float>(b)};
    for (std::uint64_t y=output.y;y<static_cast<std::uint64_t>(output.y)+output.height;++y)
        for (std::uint64_t x=output.x;x<static_cast<std::uint64_t>(output.x)+output.width;++x) {
            const auto top=std::max<std::uint64_t>(canvas.y,y>=r?y-r:0);
            const auto bottom=std::min(y+r+1,static_cast<std::uint64_t>(canvas.y)+canvas.height);
            const double center=horizontal[index(centers,x,y)];bool equal=true;double sum=0,mass=0;
            for (auto yy=top;yy<bottom;++yy) {
                const double v=horizontal[index(centers,x,yy)];equal=equal&&(v==center);
                const double w=weights_[static_cast<std::size_t>(static_cast<std::int64_t>(yy)-static_cast<std::int64_t>(y)+r)];
                const double product=w*v;sum+=product;mass+=w;
            }
            mapped.coverage[index(output,x,y)]=stored(equal?center:sum/mass);
        }
    return reduce(std::move(mapped),o,l);
}
CoverageRefineNode::CoverageRefineNode(std::shared_ptr<const CoverageNode> mask,std::shared_ptr<const Node> image,
    Rect image_bounds,const MaskRefineSettings& s):mask_(std::move(mask)),image_(std::move(image)),settings_(s) {
    input(mask_);validate_mask_refine_settings(settings_);
    if (!image_) throw std::invalid_argument("mask refinement RGB guide is null");
    extent(image_bounds);
    if (!same(image_bounds,mask_->native_bounds())) throw std::invalid_argument("mask refinement guide extent differs");
    descriptor_=image_->output_descriptor();weights_=guide_weights(descriptor_);
}
bool CoverageRefineNode::supports_level(RenderLevel l) const noexcept {return admitted(l)&&mask_->supports_level({})&&image_->supports_level({});}
Rect CoverageRefineNode::native_bounds() const noexcept {return mask_->native_bounds();}
RenderLevel CoverageRefineNode::input_level(RenderLevel l) const {return native_level(*this,l);}
Rect CoverageRefineNode::input_region_level(Rect o,Rect i,RenderLevel l) const {return region(*this,o,i,l,2*settings_.radius);}
CoverageTile CoverageRefineNode::render_level(Rect o,RenderLevel l) const {
    const auto output=block(*this,o,l),canvas=native_bounds();const auto r=settings_.radius;
    const auto centers=expand(output,canvas,r,r),request=expand(centers,canvas,r,r);
    using Coefficient=std::array<double,3>;static_assert(sizeof(Coefficient)==24);
    const auto n=count<float>(request),g=count<float>(request,3),a=r?count<Coefficient>(centers):0,b=r?count<float>(output):0,p=l.mip?count<float>(o):0;
    payload({n*sizeof(float),g*sizeof(float),a*sizeof(Coefficient),b*sizeof(float),p*sizeof(float)});
    auto mask=mask_->render_level(request,{});coverage(mask,request);
    auto rgb=image_->render_level(request,{});guide(rgb,request,descriptor_);
    if (!r) return reduce(std::move(mask),o,l);
    std::vector<Coefficient> coefficients(a);
    for (std::uint64_t y=centers.y;y<static_cast<std::uint64_t>(centers.y)+centers.height;++y)
        for (std::uint64_t x=centers.x;x<static_cast<std::uint64_t>(centers.x)+centers.width;++x) {
            const auto k=index(request,x,y);double sg=0,sp=0;unsigned samples=0;
            window(x,y,r,canvas,[&](auto xx,auto yy) {
                const auto j=index(request,xx,yy);const double d=difference(rgb,j,k,weights_);
                const double dp=static_cast<double>(mask.coverage[j])-static_cast<double>(mask.coverage[k]);
                sg+=d;sp+=dp;++samples;
            });
            const double mg=finite(sg/samples),mp=finite(sp/samples);double variance=0,covariance=0;
            window(x,y,r,canvas,[&](auto xx,auto yy) {
                const auto j=index(request,xx,yy);const double dy=difference(rgb,j,k,weights_)-mg;
                const double delta=static_cast<double>(mask.coverage[j])-static_cast<double>(mask.coverage[k]),dp=delta-mp;
                const double square=dy*dy,cross=dy*dp;variance+=square;covariance+=cross;
            });
            const double v=finite(variance/samples),c=finite(covariance/samples),denominator=v+settings_.epsilon;
            coefficients[index(centers,x,y)]={mg,mp,finite(c/denominator)};
        }
    CoverageTile mapped{output,std::vector<float>(b)};
    for (std::uint64_t y=output.y;y<static_cast<std::uint64_t>(output.y)+output.height;++y)
        for (std::uint64_t x=output.x;x<static_cast<std::uint64_t>(output.x)+output.width;++x) {
            const auto i=index(request,x,y);double sum=0;unsigned samples=0;
            window(x,y,r,canvas,[&](auto xx,auto yy) {
                const auto k=index(request,xx,yy);const auto& coeff=coefficients[index(centers,xx,yy)];
                const double delta=static_cast<double>(mask.coverage[k])-static_cast<double>(mask.coverage[i]),base=delta+coeff[1];
                double correction=base;
                if (coeff[2]!=0) {
                    const double dy=difference(rgb,i,k,weights_)-coeff[0],product=coeff[2]*dy;
                    correction=base+product;
                }
                sum+=correction;++samples;
            });
            const double correction=finite(sum/samples);
            mapped.coverage[index(output,x,y)]=correction==0?mask.coverage[i]:stored(static_cast<double>(mask.coverage[i])+correction);
        }
    return reduce(std::move(mapped),o,l);
}
} // namespace rawengine
