#include "GeometryOps.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected projective rejection");}
bool same(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size()&&(a.empty()||std::memcmp(a.data(),b.data(),a.size()*4)==0);}
using Matrix=std::array<double,9>;
const Matrix identity{1,0,0,0,1,0,0,0,1};
class Fixture final:public Node {
public:
    Rect image{17,23,257,133};WorkingSpace space=WorkingSpace::LinearProPhotoD50;
    mutable std::vector<Rect> requests;unsigned fault=0;bool constant=false;
    bool supports_level(RenderLevel l)const noexcept override{return (!l.mip&&l.quality==RenderQuality::Final)||(l.mip>=1&&l.mip<=2&&l.quality==RenderQuality::Preview);}
    ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(space);}
    Tile render(Rect r)const override{
        require(r.width<=257&&r.height<=257,"source fetch exceeds cap");requests.push_back(r);
        Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3),output_descriptor()};
        for(unsigned y=0;y<r.height;++y)for(unsigned x=0;x<r.width;++x)for(unsigned c=0;c<3;++c){
            auto xx=std::uint64_t(r.x)+x-image.x,yy=std::uint64_t(r.y)+y-image.y;
            t.rgb[(std::size_t(y)*r.width+x)*3+c]=constant?(c==0?-0.f:c==1?-.125f:1.25f):float((xx*7+yy*11+c*3)%43)/8-.5f;
        }
        if(fault==1)t.bounds.x++;
        if(fault==2)t.rgb.pop_back();
        if(fault==3)t.descriptor=ImageDescriptor::camera_linear();
        if(fault>=4)t.rgb[fault==6?(std::size_t(r.width)-1)*3:t.rgb.size()-1]=fault==5?std::numeric_limits<float>::infinity():std::numeric_limits<float>::quiet_NaN();
        return t;
    }
};
void parity(ProjectiveNode& node,unsigned W,unsigned H){
    auto native=node.render({0,0,W,H});
    for(unsigned mip=0;mip<=2;++mip){
        auto scale=1u<<mip,w=(W+scale-1)/scale,h=(H+scale-1)/scale;RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
        auto full=node.render_level({0,0,w,h},level);
        for(unsigned y=0;y<h;y+=13)for(unsigned x=0;x<w;x+=17){
            Rect r{x,y,std::min(17u,w-x),std::min(13u,h-y)};auto tile=node.render_level(r,level);
            for(unsigned row=0;row<r.height;++row)require(std::memcmp(tile.rgb.data()+std::size_t(row)*r.width*3,full.rgb.data()+(std::size_t(y+row)*w+x)*3,r.width*12)==0,"exact ROI/mip/cell splitting");
        }
        if(mip)for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<3;++c){
            double sum=0;unsigned count=0;
            for(unsigned yy=y*scale;yy<std::min((y+1)*scale,H);++yy)for(unsigned xx=x*scale;xx<std::min((x+1)*scale,W);++xx){sum+=native.rgb[(std::size_t(yy)*W+xx)*3+c];++count;}
            require(full.rgb[(std::size_t(y)*w+x)*3+c]==float(sum/count),"direct native-before-preview partial cells");
        }
    }
}
void mapping(){
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        auto source=std::make_shared<Fixture>();source->space=space;
        for(auto m:{identity,Matrix{-1,0,0,0,1,0,0,0,1},Matrix{1,.125,.1,-.125,1,-.2,.25,-.125,1},Matrix{.5,.25,.125,-.125,1.5,-.25,0,0,1}}){
            ProjectiveSettings settings{257,133,m};ProjectiveNode node(source,source->image,settings);settings.source_from_output=identity;settings.width=1;
            require(node.input_node()==source.get()&&node.output_descriptor()==source->output_descriptor(),"input/settings/descriptor ownership");
            auto full=node.render({0,0,257,133});auto original=source->render(source->image);
            if(m==identity)require(same(full.rgb,original.rgb),"identity rebased native bits");
            // Independent weighted formula and normalized matrix multiplication.
            for(unsigned y=0;y<133;y+=11)for(unsigned x=0;x<257;x+=17){
                double u=double(x)/128-1,v=double(y)/66-1,d=m[6]*u+m[7]*v+1;
                double sx=std::clamp((m[0]*u+m[1]*v+m[2]+d)*128/d,0.,256.);
                double sy=std::clamp((m[3]*u+m[4]*v+m[5]+d)*66/d,0.,132.);
                unsigned ix=unsigned(std::floor(sx)),iy=unsigned(std::floor(sy)),jx=std::min(ix+1,256u),jy=std::min(iy+1,132u);double fx=sx-ix,fy=sy-iy;
                for(unsigned c=0;c<3;++c){auto at=[&](unsigned xx,unsigned yy){return double(original.rgb[(std::size_t(yy)*257+xx)*3+c]);};
                    double expected=(1-fx)*(1-fy)*at(ix,iy)+fx*(1-fy)*at(jx,iy)+(1-fx)*fy*at(ix,jy)+fx*fy*at(jx,jy);
                    require(std::abs(double(full.rgb[(std::size_t(y)*257+x)*3+c])-expected)<2e-6,"independent projective four-weight truth");
                }
            }
            parity(node,257,133);
            source->constant=true;auto constant=node.render({41,31,13,11});source->constant=false;
            for(std::size_t i=0;i<constant.rgb.size();i+=3)require(std::signbit(constant.rgb[i])&&constant.rgb[i+1]==-.125f&&constant.rgb[i+2]==1.25f,"signed constant component bits");
        }
        source->image={17,23,4097,1025};ProjectiveNode amplified(source,source->image,{129,37,identity});parity(amplified,129,37);
        require(source->requests.size()>100,"amplified support subdivision");
        source->image={0,0,UINT32_MAX,UINT32_MAX};ProjectiveNode virtual_large(source,source->image,{9,7,identity});parity(virtual_large,9,7);
        auto r=virtual_large.input_region({0,0,9,7},source->image);require(r.width==UINT32_MAX&&r.height==UINT32_MAX,"virtual full uint32 support planning");
    }
    for(auto wh:{std::pair{1u,1u},std::pair{1u,7u},std::pair{9u,1u},std::pair{5u,4u}}){
        std::vector<float> pixels(wh.first*wh.second*3);for(std::size_t i=0;i<pixels.size();++i)pixels[i]=float(i)/8;
        auto source=std::make_shared<RasterSourceNode>(RasterImage({wh.first,wh.second,0,WorkingSpace::LinearProPhotoD50},pixels));
        ProjectiveNode node(source,{0,0,wh.first,wh.second},{7,5,{1,.125,0,0,1,0,.25,0,1}});parity(node,7,5);
        auto a=std::async(std::launch::async,[&]{return node.render({0,0,7,5});});auto b=std::async(std::launch::async,[&]{return node.render({0,0,7,5});});require(same(a.get().rgb,b.get().rgb),"concurrent immutable mapping");
    }
    std::vector<float> ramp;for(unsigned x=0;x<9;++x)for(unsigned c=0;c<3;++c)ramp.push_back(float(x));
    auto source=std::make_shared<RasterSourceNode>(RasterImage({9,1,0,WorkingSpace::LinearProPhotoD50},ramp));
    auto resized=ProjectiveNode(source,{0,0,9,1},{5,1,identity}).render({0,0,5,1});
    for(unsigned x=0;x<5;++x)require(resized.rgb[x*3]==2*float(x),"endpoint aligned canvas truth");
}
void validation(){
    rejects([]{validate_projective_settings({});});
    for(auto value:{17.,-17.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}){auto m=identity;m[2]=value;rejects([&]{validate_projective_settings({1,1,m});});}
    for(auto m:{Matrix{0,0,0,0,1,0,0,0,1},Matrix{1,0,0,0,1,0,.75001,0,1},Matrix{1,0,0,0,1,0,0,0,2}})rejects([&]{validate_projective_settings({1,1,m});});
    for(double d:{0x1p-20,-0x1p-20}){auto m=identity;m[0]=d;validate_projective_settings({1,1,m});m[0]=std::nextafter(d,0.);rejects([&]{validate_projective_settings({1,1,m});});}
    auto boundary=identity;boundary[6]=.75;validate_projective_settings({1,1,boundary});
    auto f=std::make_shared<Fixture>();rejects([&]{ProjectiveNode n(nullptr,f->image,{1,1,identity});});rejects([&]{ProjectiveNode n(f,{0,0,0,1},{1,1,identity});});rejects([&]{ProjectiveNode n(f,{UINT32_MAX,0,2,1},{1,1,identity});});
    rejects([]{ProjectiveNode n(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})),{0,0,1,1},{1,1,identity});});
    ProjectiveNode node(f,f->image,{257,133,identity});
    for(auto l:{RenderLevel{0,RenderQuality::Preview},RenderLevel{1,RenderQuality::Final},RenderLevel{3,RenderQuality::Preview}})rejects([&]{node.render_level({0,0,1,1},l);});
    rejects([&]{node.input_region({0,0,1,1},{0,0,257,133});});rejects([&]{node.render({257,0,1,1});});
    require(node.render({257,133,0,0}).rgb.empty()&&f->requests.empty(),"empty without fetch");
    for(unsigned fault=1;fault<=5;++fault){f->fault=fault;rejects([&]{node.render({0,0,3,3});});}f->fault=0;
    f->image={0,0,8193,8193};f->fault=6;
    ProjectiveNode diagonal(f,f->image,{129,1,{1,0,0,1,1,0,0,0,1}});
    rejects([&]{diagonal.render({62,0,5,1});}); // Poison a fetched hole outside active diagonal taps.
    f->fault=0;
    f->image={UINT32_MAX,UINT32_MAX,1,1};ProjectiveNode last(f,f->image,{1,1,identity});require(last.render({0,0,1,1}).rgb.size()==3,"exclusive uint32 end");auto region=last.input_region({0,0,1,1},f->image);require(region.x==UINT32_MAX&&region.y==UINT32_MAX&&region.width==1,"last pixel support");
    for(float value:{std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),std::numeric_limits<float>::denorm_min(),-std::numeric_limits<float>::denorm_min(),-0.f}){
        std::vector<float> pixels(27,value);auto source=std::make_shared<RasterSourceNode>(RasterImage({3,3,0,WorkingSpace::LinearProPhotoD50},pixels));
        require(same(ProjectiveNode(source,{0,0,3,3},{3,3,{1,.125,0,0,1,0,.25,0,1}}).render({0,0,3,3}).rgb,pixels),"finite extreme constants");
    }
}
}
int main(){try{mapping();validation();std::cout<<"Projective native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
