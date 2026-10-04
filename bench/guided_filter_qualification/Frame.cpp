#include "SpatialOps.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
using namespace rawengine;
using Clock=std::chrono::steady_clock;
double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
double median(std::vector<double> x){std::sort(x.begin(),x.end());return x[x.size()/2];}
void require(bool v,const char* s){if(!v)throw std::runtime_error(s);}
bool same(Rect a,Rect b){return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;}
struct Counters{
 double source_ms=0;std::uint64_t calls=0,bytes=0,max_pixels=0,signature=1469598103934665603ull;
 void add(Rect r,RenderLevel l){++calls;auto pixels=std::uint64_t(r.width)*r.height;bytes+=pixels*12;max_pixels=std::max(max_pixels,pixels);for(auto v:{r.x,r.y,r.width,r.height,l.mip}){signature^=v;signature*=1099511628211ull;}}
};
class MeasuredSource final:public Node{
 std::shared_ptr<const Node> base_;
public:
 mutable Counters counters;
 explicit MeasuredSource(std::shared_ptr<const Node> p):base_(std::move(p)){}
 Tile render(Rect r)const override{return render_level(r,{});}
 Tile render_level(Rect r,RenderLevel l)const override{auto start=Clock::now();auto t=base_->render_level(r,l);counters.source_ms+=ms(Clock::now()-start);counters.add(r,l);return t;}
 bool supports_level(RenderLevel l)const noexcept override{return base_->supports_level(l);}
 ImageDescriptor output_descriptor()const noexcept override{return base_->output_descriptor();}
};
std::vector<Rect> partitions(Rect image,unsigned tile){std::vector<Rect> r;for(unsigned y=0;y<image.height;y+=tile)for(unsigned x=0;x<image.width;x+=tile)r.push_back({x,y,std::min(tile,image.width-x),std::min(tile,image.height-y)});return r;}
Rect clipped_expand(Rect r,Rect image,unsigned halo){
 auto left=r.x>halo?r.x-halo:0u,top=r.y>halo?r.y-halo:0u;
 auto right=std::min<std::uint64_t>(image.width,std::uint64_t(r.x)+r.width+halo);
 auto bottom=std::min<std::uint64_t>(image.height,std::uint64_t(r.y)+r.height+halo);
 return {left,top,unsigned(right-left),unsigned(bottom-top)};
}
int main(int argc,char** argv){try{
 require(argc==2,"fresh output directory required");std::filesystem::path output=argv[1];require(!std::filesystem::exists(output),"preserve prior report");std::filesystem::create_directories(output);
 std::ofstream report(output/"measurements.json");report<<std::setprecision(17)<<"{\"cases\":[";bool first=true;unsigned fixtures=0,exact=0,cancel_checks=0,helper_checks=0;std::uint64_t support_checks=0;
 constexpr unsigned w=1025,h=769,repeats=3;Renderer renderer;
 for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
  std::vector<float> samples(std::size_t(w)*h*3);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<3;++c)samples[(std::size_t(y)*w+x)*3+c]=float(int((x*19+y*37+c*83)%257)-64)/128;
  auto raster=std::make_shared<RasterSourceNode>(RasterImage({w,h,0,space},std::move(samples)));
  auto source=std::make_shared<MeasuredSource>(raster);
  for(const auto settings:{WorkingYGuidedFilterSettings{0,0x1p-24},WorkingYGuidedFilterSettings{3,0x1p-12},WorkingYGuidedFilterSettings{8,0x1p-24},WorkingYGuidedFilterSettings{8,65536.0}}){
   auto node=std::make_shared<WorkingYGuidedFilterNode>(source,Rect{0,0,w,h},settings);
   for(unsigned mip=0;mip<=2;++mip){unsigned scale=1u<<mip;Rect image{0,0,(w+scale-1)/scale,(h+scale-1)/scale};RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
    auto reduced=raster->render_level(image,level);auto reference=working_y_guided_filter_rgb(reduced,image,image,settings);reduced.rgb.clear();reduced.rgb.shrink_to_fit();
    auto direct=node->render_level(image,level);require(same(direct.bounds,reference.bounds)&&direct.descriptor==reference.descriptor&&direct.rgb==reference.rgb,"helper/node full frame mismatch");++helper_checks;direct.rgb.clear();direct.rgb.shrink_to_fit();
    std::string name=std::to_string(unsigned(space))+"-r"+std::to_string(settings.radius)+(settings.epsilon==65536.?"-flat":"-tight")+"-m"+std::to_string(mip)+".f32";std::ofstream bytes(output/name,std::ios::binary);bytes.write(reinterpret_cast<const char*>(reference.rgb.data()),reference.rgb.size()*4);bytes.close();
    for(const std::string mode:{"direct","image256","image64","stream256"}){
     bool is_direct=mode=="direct",stream=mode=="stream256";unsigned tile=mode=="image64"?64u:256u;auto rects=is_direct?std::vector<Rect>{image}:partitions(image,tile);Counters expected;std::uint64_t max_node_bytes=0,coefficient_centers=0,max_coefficients=0,delivered=0;
     for(auto rect:rects){auto needed=clipped_expand(rect,image,2*settings.radius);require(same(node->input_region_level(rect,image,level),needed),"2r support differs from independent expansion");++support_checks;expected.add(needed,level);auto centers=clipped_expand(rect,image,settings.radius);auto n=std::uint64_t(needed.width)*needed.height,o=std::uint64_t(rect.width)*rect.height,a=settings.radius?std::uint64_t(centers.width)*centers.height:0;
      coefficient_centers+=a;max_coefficients=std::max(max_coefficients,a);max_node_bytes=std::max(max_node_bytes,12*n+56*a+12*o);
     }
     std::vector<double> totals,sources,callbacks;Counters stable;
     auto run=[&](){double callback_ms=0;std::uint64_t pixels=0;std::size_t index=0;source->counters={};auto start=Clock::now();
      if(stream)renderer.render_tiles(*node,{0,0,w,h},RenderRequest{image,tile,level},[&](const Tile& value){auto begin=Clock::now();require(index<rects.size()&&same(value.bounds,rects[index++]),"stream tile bounds/order");require(value.descriptor==reference.descriptor,"stream descriptor");for(unsigned y=0;y<value.bounds.height;++y){auto offset=(std::size_t(value.bounds.y+y)*image.width+value.bounds.x)*3;require(std::memcmp(reference.rgb.data()+offset,value.rgb.data()+std::size_t(y)*value.bounds.width*3,std::size_t(value.bounds.width)*12)==0,"stream differs from full helper");}pixels+=std::uint64_t(value.bounds.width)*value.bounds.height;callback_ms+=ms(Clock::now()-begin);});
      else {auto value=is_direct?node->render_level(image,level):renderer.render_image(*node,{0,0,w,h},RenderRequest{image,tile,level});double elapsed=ms(Clock::now()-start);require(same(value.bounds,image)&&value.descriptor==reference.descriptor&&value.rgb.size()==reference.rgb.size()&&std::memcmp(value.rgb.data(),reference.rgb.data(),value.rgb.size()*4)==0,"assembled differs from full helper");delivered=std::uint64_t(image.width)*image.height;return std::pair<double,double>{elapsed,0};}
      require(index==rects.size()&&pixels==std::uint64_t(image.width)*image.height,"stream coverage");delivered=pixels;return std::pair<double,double>{ms(Clock::now()-start),callback_ms};
     };
     run();for(unsigned repeat=0;repeat<repeats;++repeat){auto clocks=run();auto c=source->counters;require(c.calls==expected.calls&&c.bytes==expected.bytes&&c.max_pixels==expected.max_pixels&&c.signature==expected.signature,"source request/order mismatch");totals.push_back(clocks.first);sources.push_back(c.source_ms);callbacks.push_back(clocks.second);stable=c;++exact;}
     if(!first)report<<',';first=false;++fixtures;report<<"{\"space\":"<<unsigned(space)<<",\"radius\":"<<settings.radius<<",\"epsilon\":"<<settings.epsilon<<",\"mip\":"<<mip<<",\"mode\":\""<<mode<<"\",\"output\":\""<<name<<"\",\"output_pixels\":"<<delivered<<",\"source_calls\":"<<stable.calls<<",\"source_bytes\":"<<stable.bytes<<",\"max_source_pixels\":"<<stable.max_pixels<<",\"source_request_signature\":"<<stable.signature<<",\"coefficient_centers\":"<<coefficient_centers<<",\"max_coefficient_centers\":"<<max_coefficients<<",\"derived_node_logical_bytes\":"<<max_node_bytes<<",\"derived_renderer_retained_bytes\":"<<(is_direct||stream?0:std::uint64_t(image.width)*image.height*12)<<",\"median_total_ms\":"<<median(totals)<<",\"median_source_ms\":"<<median(sources)<<",\"median_callback_validation_ms\":"<<median(callbacks)<<",\"total_samples_ms\":[";
     for(unsigned i=0;i<totals.size();++i){if(i)report<<',';report<<totals[i];}report<<"],\"source_samples_ms\":[";for(unsigned i=0;i<sources.size();++i){if(i)report<<',';report<<sources[i];}report<<"],\"callback_samples_ms\":[";for(unsigned i=0;i<callbacks.size();++i){if(i)report<<',';report<<callbacks[i];}report<<"]}";report.flush();
    }
    CancellationToken before;before.cancel();source->counters={};unsigned calls=0;bool rejected=false;
    try{renderer.render_tiles(*node,{0,0,w,h},RenderRequest{image,256,level},[&](const Tile&){++calls;},&before);}catch(const RenderCancelled&){rejected=true;}
    require(rejected&&calls==0&&source->counters.calls==0,"pre-cancel work");++cancel_checks;
    CancellationToken during;source->counters={};calls=0;rejected=false;
    try{renderer.render_tiles(*node,{0,0,w,h},RenderRequest{image,256,level},[&](const Tile&){++calls;during.cancel();},&during);}catch(const RenderCancelled&){rejected=true;}
    require(rejected&&calls==1&&source->counters.calls==1,"cancel should stop after first tile");++cancel_checks;
    std::cout<<name<<" complete\n"<<std::flush;
   }
  }
 }
 report<<"],\"complete\":true,\"source_shape\":[1025,769],\"repeats\":"<<repeats<<",\"fixtures\":"<<fixtures<<",\"exact_repeated_outputs\":"<<exact<<",\"cancellation_checks\":"<<cancel_checks<<",\"helper_node_comparisons\":"<<helper_checks<<",\"independent_support_checks\":"<<support_checks;
#ifdef _WIN32
 PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=0,"process counter failed");report<<",\"process_lifetime_peak_working_set_bytes\":"<<memory.PeakWorkingSetSize<<",\"process_lifetime_peak_commit_bytes\":"<<memory.PeakPagefileUsage;
 wchar_t dll_path[32768]{};require(GetModuleFileNameW(GetModuleHandleW(L"RawEngine.dll"),dll_path,32768)>0,"loaded DLL path failed");report<<",\"loaded_dll\":\""<<std::filesystem::path(dll_path).generic_string()<<"\"";
#endif
 report<<"}\n";std::cout<<fixtures<<" cases,"<<exact<<" exact repeats,"<<cancel_checks<<" cancellations,"<<support_checks<<" support checks\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
