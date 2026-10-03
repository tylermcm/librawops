#include "GeometryOps.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
template<class F>void rejects(F f) { try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected rotate rejection"); }
bool same(const std::vector<float>& a,const std::vector<float>& b) { return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0); }
class Fixture final:public Node {
public:
    Rect image{17,23,257,133};WorkingSpace space=WorkingSpace::LinearProPhotoD50;
    mutable std::vector<Rect> requests;unsigned fault=0;bool constant=false;
    ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(space);}
    bool supports_level(RenderLevel l)const noexcept override{return (l.mip==0 && l.quality==RenderQuality::Final)||(l.mip>=1 && l.mip<=2 && l.quality==RenderQuality::Preview);}
    Tile render(Rect r)const override {
        requests.push_back(r);Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3),output_descriptor()};
        for(unsigned y=0;y<r.height;++y)for(unsigned x=0;x<r.width;++x)for(unsigned c=0;c<3;++c) {
            auto xx=std::uint64_t(r.x)+x-image.x,yy=std::uint64_t(r.y)+y-image.y;
            t.rgb[(std::size_t(y)*r.width+x)*3+c]=constant ? (c==0 ? -0.f : c==1 ? -.125f : 1.25f) : float((xx*7+yy*11+c*3)%43)/8-.5f;
        }
        if(fault==1)t.bounds.x++;
        if(fault==2)t.rgb.pop_back();
        if(fault==3)t.descriptor=ImageDescriptor::camera_linear();
        if(fault>=4)t.rgb[t.rgb.size()-1-(fault-4)%3]=fault<7 ? std::numeric_limits<float>::quiet_NaN() : fault<10 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
        return t;
    }
};
void controls() {
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        auto input=std::make_shared<Fixture>();input->space=space;
        const Rect output{0,0,257,133};RotateSettings settings{7.5};RotateNode node(input,input->image,settings);settings.angle_degrees=0;
        auto full=node.render(output);require(node.input_node()==input.get() && node.output_descriptor()==input->output_descriptor(),"owned input/settings/descriptor");
        for(auto r:input->requests)require(r.width<=257 && r.height<=257,"source block resource extent");
        require(input->requests.size()>1,"multi-block rendering");
        // Independent four-weight interpolation, with a different arithmetic
        // formulation from the ordered production blend.
        auto source=input->render(input->image);const double pi=0x1.921fb54442d18p+1;
        const double c=std::cos((7.5/180)*pi),s=std::sin((7.5/180)*pi),cx=128,cy=66;
        for(unsigned y=0;y<133;y+=7)for(unsigned x=0;x<257;x+=11) {
            const double sx=std::clamp(cx+c*(x-cx)+s*(y-cy),0.,256.),sy=std::clamp(cy-s*(x-cx)+c*(y-cy),0.,132.);
            const unsigned ix=unsigned(std::floor(sx)),iy=unsigned(std::floor(sy)),jx=std::min(ix+1,256u),jy=std::min(iy+1,132u);const double fx=sx-ix,fy=sy-iy;
            for(unsigned channel=0;channel<3;++channel) {
                auto v=[&](unsigned xx,unsigned yy){return double(source.rgb[(std::size_t(yy)*257+xx)*3+channel]);};
                const double expected=(1-fx)*(1-fy)*v(ix,iy)+fx*(1-fy)*v(jx,iy)+(1-fx)*fy*v(ix,jy)+fx*fy*v(jx,jy);
                require(std::abs(double(full.rgb[(std::size_t(y)*257+x)*3+channel])-expected)<4e-7,"independent weighted bilinear truth");
            }
        }
        for(unsigned mip=0;mip<=2;++mip) {
            const unsigned scale=1u<<mip,w=(257+scale-1)/scale,h=(133+scale-1)/scale;RenderLevel level{mip,mip ? RenderQuality::Preview : RenderQuality::Final};
            auto whole=node.render_level({0,0,w,h},level);
            for(unsigned y=0;y<h;y+=31)for(unsigned x=0;x<w;x+=37) {
                Rect r{x,y,std::min(37u,w-x),std::min(31u,h-y)};auto tile=node.render_level(r,level);
                for(unsigned row=0;row<r.height;++row)require(std::memcmp(tile.rgb.data()+std::size_t(row)*r.width*3,whole.rgb.data()+(std::size_t(y+row)*w+x)*3,r.width*12)==0,"ROI/mip/block exact parity");
            }
            if(mip)for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned channel=0;channel<3;++channel) {
                double sum=0;unsigned count=0;
                for(unsigned yy=y*scale;yy<std::min((y+1)*scale,133u);++yy)for(unsigned xx=x*scale;xx<std::min((x+1)*scale,257u);++xx){sum+=full.rgb[(std::size_t(yy)*257+xx)*3+channel];++count;}
                require(whole.rgb[(std::size_t(y)*w+x)*3+channel]==float(sum/count),"native-before-preview row-major truth");
            }
        }
        auto identity=RotateNode(input,input->image).render(output);require(same(identity.rgb,source.rgb),"native rebased identity bits");
        auto region=RotateNode(input,input->image).input_region({125,129,2,3},input->image);require(region.x==142 && region.y==152 && region.width==2 && region.height==3,"identity no extra taps");
        input->constant=true;for(auto angle:{-180.,-90.,-.1,0.,.1,90.,180.}) {
            auto result=RotateNode(input,input->image,{angle}).render({113,7,19,11});
            for(std::size_t i=0;i<result.rgb.size();i+=3)require(result.rgb[i]==0 && std::signbit(result.rgb[i]) && result.rgb[i+1]==-.125f && result.rgb[i+2]==1.25f,"constant signed/headroom bits");
        }
        input->constant=false;
    }
    for(unsigned N:{1u,7u,8u}) {
        std::vector<float> data(std::size_t(N)*N*3);for(std::size_t i=0;i<data.size();++i)data[i]=float(i)/8;
        auto input=std::make_shared<RasterSourceNode>(RasterImage({N,N,0,WorkingSpace::LinearProPhotoD50},data));
        for(auto a:{-180.,-90.,0.,90.,180.}) {
            auto image=RotateNode(input,{0,0,N,N},{a}).render({0,0,N,N});
            for(unsigned y=0;y<N;++y)for(unsigned x=0;x<N;++x) {
                unsigned xx=x,yy=y;if(a==90){xx=y;yy=N-1-x;}if(a==-90){xx=N-1-y;yy=x;}if(std::abs(a)==180){xx=N-1-x;yy=N-1-y;}
                require(std::memcmp(image.rgb.data()+(std::size_t(y)*N+x)*3,data.data()+(std::size_t(yy)*N+xx)*3,12)==0,"square quarter coefficient permutation");
            }
        }
        RotateNode node(input,{0,0,N,N},{13.});auto f=std::async(std::launch::async,[&]{return node.render({0,0,N,N});});auto g=std::async(std::launch::async,[&]{return node.render({0,0,N,N});});require(same(f.get().rgb,g.get().rgb),"concurrent immutable render");
    }
}
void validation() {
    for(auto a:{-180.01,180.01,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})rejects([&]{validate_rotate_settings({a});});
    rejects([]{RotateNode n(nullptr,{0,0,1,1});});auto f=std::make_shared<Fixture>();
    rejects([&]{RotateNode n(f,{0,0,0,1});});rejects([&]{RotateNode n(f,{UINT32_MAX,0,2,1});});
    rejects([]{RotateNode n(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})),{0,0,1,1});});
    RotateNode node(f,f->image,{7.5});for(auto level:{RenderLevel{0,RenderQuality::Preview},RenderLevel{1,RenderQuality::Final},RenderLevel{3,RenderQuality::Preview}})rejects([&]{node.render_level({0,0,1,1},level);});
    rejects([&]{node.input_region({0,0,1,1},{0,0,257,133});});rejects([&]{node.render({257,0,1,1});});
    auto empty=node.render({257,133,0,0});require(empty.rgb.empty() && f->requests.empty(),"empty render without fetch");
    for(unsigned fault=1;fault<13;++fault) {f->fault=fault;for(auto angle:{0.,7.5})rejects([&]{RotateNode(f,f->image,{angle}).render({100,60,3,3});});}
    f->fault=0;f->image={UINT32_MAX,UINT32_MAX,1,1};auto end=RotateNode(f,f->image,{45.});require(end.render({0,0,1,1}).rgb.size()==3,"last uint32 source pixel");auto r=end.input_region({0,0,1,1},f->image);require(r.x==UINT32_MAX && r.y==UINT32_MAX && r.width==1 && r.height==1,"exclusive end 2^32");
    const float max=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
    for(float value:{max,-max,tiny,-tiny,-0.f}) {
        std::vector<float> data(27,value);auto input=std::make_shared<RasterSourceNode>(RasterImage({3,3,0,WorkingSpace::LinearProPhotoD50},data));
        require(same(RotateNode(input,{0,0,3,3},{7.5}).render({0,0,3,3}).rgb,data),"finite extreme constant bits");
    }
}
}
int main(){try{controls();validation();std::cout<<"Rotate native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
