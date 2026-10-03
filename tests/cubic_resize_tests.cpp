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
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected cubic resize rejection");}
bool same(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size()&&(a.empty()||std::memcmp(a.data(),b.data(),a.size()*4)==0);}
class Fixture final:public Node {
public:
    Rect image{17,23,257,133};WorkingSpace space=WorkingSpace::LinearProPhotoD50;
    mutable std::vector<Rect> requests;unsigned fault=0;bool constant=false,preview=true;
    bool supports_level(RenderLevel l)const noexcept override{return (!l.mip&&l.quality==RenderQuality::Final)||(preview&&l.mip>=1&&l.mip<=2&&l.quality==RenderQuality::Preview);}
    ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(space);}
    Tile render(Rect r)const override{
        require(r.width<=145&&r.height<=145,"source fetch exceeds cap");requests.push_back(r);
        Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3),output_descriptor()};
        for(unsigned y=0;y<r.height;++y)for(unsigned x=0;x<r.width;++x)for(unsigned c=0;c<3;++c){
            auto xx=std::uint64_t(r.x)+x-image.x,yy=std::uint64_t(r.y)+y-image.y;
            t.rgb[(std::size_t(y)*r.width+x)*3+c]=constant?(c==0?-0.f:c==1?-.125f:1.25f):float((xx*7+yy*11+c*3)%43)/8-.5f;
        }
        if(fault==1)t.bounds.x++;
        if(fault==2)t.rgb.pop_back();
        if(fault==3)t.descriptor=ImageDescriptor::camera_linear();
        if(fault>=4&&fault<=6)t.rgb[fault==6?(std::size_t(r.width)-1)*3:t.rgb.size()-1]=fault==5?std::numeric_limits<float>::infinity():std::numeric_limits<float>::quiet_NaN();
        if(fault==7)t.rgb[(std::size_t(image.x)+1-r.x)*3]=std::numeric_limits<float>::quiet_NaN();
        return t;
    }
};
void parity(CubicResizeNode& node,unsigned W,unsigned H){
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
std::vector<std::pair<unsigned,long double>> truth_axis(unsigned N,unsigned O,unsigned index){
    if(N==O)return {{index,1}};
    const long double z=std::clamp((static_cast<long double>(2ull*index+1)*N)/(2*static_cast<long double>(O))-.5L,0.L,static_cast<long double>(N-1));
    const long double s=std::max(1.L,static_cast<long double>(N)/O);std::vector<std::pair<unsigned,long double>> taps;long double sum=0;
    for(long long i=static_cast<long long>(std::floor(z-2*s));i<=static_cast<long long>(std::ceil(z+2*s));++i){
        const long double t=std::abs((i-z)/s),w=(t<1?1-2.5L*t*t+1.5L*t*t*t:t<2?2-4*t+2.5L*t*t-.5L*t*t*t:0)/s;
        if(w!=0){taps.emplace_back(unsigned(std::clamp(i,0ll,static_cast<long long>(N)-1)),w);sum+=w;}
    }
    for(auto& p:taps)p.second/=sum;return taps;
}
void mapping(){
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        auto source=std::make_shared<Fixture>();source->space=space;source->image={17,23,129,65};
        for(auto wh:{std::pair{129u,65u},std::pair{193u,97u},std::pair{33u,17u},std::pair{129u,97u}}){
            CubicResizeSettings settings{wh.first,wh.second};CubicResizeNode node(source,source->image,settings);settings.width=1;
            require(node.input_node()==source.get()&&node.output_descriptor()==source->output_descriptor(),"copied input/settings/descriptor");
            auto full=node.render({0,0,wh.first,wh.second});
            for(unsigned y=0;y<wh.second;y+=7)for(unsigned x=0;x<wh.first;x+=13){
                auto xs=truth_axis(129,wh.first,x),ys=truth_axis(65,wh.second,y);
                for(unsigned c=0;c<3;++c){long double expected=0;for(auto [xx,wx]:xs)for(auto [yy,wy]:ys)expected+=wx*wy*(static_cast<long double>((xx*7+yy*11+c*3)%43)/8-.5L);
                    require(std::abs(static_cast<long double>(full.rgb[(std::size_t(y)*wh.first+x)*3+c])-expected)<2e-6,"independent expanded tensor-weight truth");}
            }
            parity(node,wh.first,wh.second);
            source->constant=true;auto constant=node.render({0,0,wh.first,wh.second});source->constant=false;
            for(std::size_t i=0;i<constant.rgb.size();i+=3)require(std::signbit(constant.rgb[i])&&constant.rgb[i+1]==-.125f&&constant.rgb[i+2]==1.25f,"signed constant bits");
        }
        source->image={0,0,UINT32_MAX,UINT32_MAX};const auto out=UINT32_MAX/4+1;
        CubicResizeNode huge(source,source->image,{out,out});auto region=huge.input_region({out-32,out-32,32,32},source->image);
        require(region.width<=145&&region.height<=145,"virtual uint32 shrink support cap");
        require(huge.render({out-32,out-32,32,32}).rgb.size()==32*32*3,"virtual uint32 bounded fetch");
        auto preview=huge.render_level({out/4-2,out/4-2,2,2},{2,RenderQuality::Preview});require(preview.rgb.size()==12,"virtual uint32 mip native mapping");
    }
    for(auto wh:{std::pair{1u,1u},std::pair{1u,7u},std::pair{9u,1u},std::pair{5u,4u}}){
        std::vector<float> pixels(wh.first*wh.second*3);for(std::size_t i=0;i<pixels.size();++i)pixels[i]=float(i)/8;
        auto source=std::make_shared<RasterSourceNode>(RasterImage({wh.first,wh.second,0,WorkingSpace::LinearProPhotoD50},pixels));
        CubicResizeNode identity(source,{0,0,wh.first,wh.second},{wh.first,wh.second});require(same(identity.render({0,0,wh.first,wh.second}).rgb,pixels),"native identity bytes");
        CubicResizeNode node(source,{0,0,wh.first,wh.second},{17,13});parity(node,17,13);
        auto a=std::async(std::launch::async,[&]{return node.render({0,0,17,13});});auto b=std::async(std::launch::async,[&]{return node.render({0,0,17,13});});require(same(a.get().rgb,b.get().rgb),"concurrent immutable mapping");
    }
}
void validation(){
    rejects([]{validate_cubic_resize_settings({}, {0,0,1,1});});
    rejects([]{validate_cubic_resize_settings({1,1},{0,0,5,1});});
    rejects([]{validate_cubic_resize_settings({1,1},{0,0,1,5});});
    validate_cubic_resize_settings({1,1},{0,0,4,4});
    auto f=std::make_shared<Fixture>();f->image={17,23,129,65};
    rejects([&]{CubicResizeNode n(nullptr,f->image,{129,65});});rejects([&]{CubicResizeNode n(f,{0,0,0,1},{1,1});});rejects([&]{CubicResizeNode n(f,{UINT32_MAX,0,2,1},{1,1});});
    rejects([]{CubicResizeNode n(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})),{0,0,1,1},{1,1});});
    CubicResizeNode node(f,f->image,{129,65});
    for(auto l:{RenderLevel{0,RenderQuality::Preview},RenderLevel{1,RenderQuality::Final},RenderLevel{3,RenderQuality::Preview}})rejects([&]{node.render_level({0,0,1,1},l);});
    rejects([&]{node.input_region({0,0,1,1},{0,0,129,65});});rejects([&]{node.render({129,0,1,1});});
    require(node.render({129,65,0,0}).rgb.empty()&&f->requests.empty(),"empty without fetch");
    for(unsigned fault=1;fault<=5;++fault){f->fault=fault;rejects([&]{node.render({0,0,3,3});});}f->fault=0;
    f->preview=false;require(!node.supports_level({1,RenderQuality::Preview}),"requested upstream level admission");rejects([&]{node.render_level({0,0,1,1},{1,RenderQuality::Preview});});f->preview=true;
    f->image={17,23,12,1};f->fault=7;
    rejects([&]{CubicResizeNode(f,f->image,{4,1}).render({1,0,1,1});}); // Source column1 is an exact zero-weight hole inside fetched [0,9].
    f->fault=0;
    f->image={UINT32_MAX,UINT32_MAX,1,1};CubicResizeNode last(f,f->image,{1,1});require(last.render({0,0,1,1}).rgb.size()==3,"exclusive uint32 end");auto region=last.input_region({0,0,1,1},f->image);require(region.x==UINT32_MAX&&region.y==UINT32_MAX&&region.width==1,"last pixel support");
    for(float value:{std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),std::numeric_limits<float>::denorm_min(),-std::numeric_limits<float>::denorm_min(),-0.f}){
        std::vector<float> pixels(27,value);auto source=std::make_shared<RasterSourceNode>(RasterImage({3,3,0,WorkingSpace::LinearProPhotoD50},pixels));
        auto output=CubicResizeNode(source,{0,0,3,3},{7,5}).render({0,0,7,5});for(float v:output.rgb)require(!std::memcmp(&v,&value,4),"finite extreme constants");
    }
    std::vector<float> spike(12,-std::numeric_limits<float>::max());spike[3]=spike[4]=spike[5]=std::numeric_limits<float>::max();
    auto source=std::make_shared<RasterSourceNode>(RasterImage({4,1,0,WorkingSpace::LinearProPhotoD50},spike));
    rejects([&]{CubicResizeNode(source,{0,0,4,1},{9,1}).render({0,0,9,1});});
}
}
int main(){try{mapping();validation();std::cout<<"Cubic resize native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
