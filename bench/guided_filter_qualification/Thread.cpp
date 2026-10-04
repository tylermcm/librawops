#include "SpatialOps.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
using namespace rawengine;using Clock=std::chrono::steady_clock;
double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
double median(std::vector<double> x){std::sort(x.begin(),x.end());return x[x.size()/2];}
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
bool same(Rect a,Rect b){return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;}
std::uint64_t expanded_pixels(Rect roi,Rect image,unsigned halo){auto x=roi.x>halo?roi.x-halo:0u,y=roi.y>halo?roi.y-halo:0u;auto right=std::min<std::uint64_t>(image.width,std::uint64_t(roi.x)+roi.width+halo),bottom=std::min<std::uint64_t>(image.height,std::uint64_t(roi.y)+roi.height+halo);return (right-x)*(bottom-y);}
int main(int argc,char** argv){try{
 require(argc==2&&!std::filesystem::exists(argv[1]),"fresh report path required");std::ofstream report(argv[1]);report<<std::setprecision(17)<<"{\"cases\":[";bool first=true;unsigned comparisons=0,batches=0;
 constexpr unsigned w=1025,h=769,tasks=8,repeats=7;
 for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
  std::vector<float> samples(std::size_t(w)*h*3);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<3;++c)samples[(std::size_t(y)*w+x)*3+c]=float(int((x*19+y*37+c*83)%257)-64)/128;
  auto source=std::make_shared<RasterSourceNode>(RasterImage({w,h,0,space},std::move(samples)));
  for(auto settings:{WorkingYGuidedFilterSettings{0,0x1p-24},WorkingYGuidedFilterSettings{3,0x1p-12},WorkingYGuidedFilterSettings{8,0x1p-24}}){
   auto node=std::make_shared<WorkingYGuidedFilterNode>(source,Rect{0,0,w,h},settings);
   for(unsigned mip=0;mip<=2;++mip){auto scale=1u<<mip;Rect image{0,0,(w+scale-1)/scale,(h+scale-1)/scale};RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
    std::array<Rect,tasks> rects{{{0,0,128,128},{image.width-128,0,128,128},{0,image.height-128,128,128},{image.width-128,image.height-128,128,128},{32,16,128,128},{64,32,128,128},{image.width/2-64,image.height/2-64,128,128},{image.width-144,image.height-144,128,128}}};
    std::array<Tile,tasks> expected;std::uint64_t max_payload=0,source_bytes=0;for(unsigned k=0;k<tasks;++k){expected[k]=node->render_level(rects[k],level);auto n=expanded_pixels(rects[k],image,2*settings.radius),a=settings.radius?expanded_pixels(rects[k],image,settings.radius):0;source_bytes+=12*n;max_payload=std::max(max_payload,12*n+56*a+12*128*128);}
    for(unsigned workers:{1u,2u,4u}){
     std::vector<double> elapsed;std::vector<unsigned> peaks;
     for(unsigned repeat=0;repeat<repeats+1;++repeat){
      std::promise<void> signal;auto gate=signal.get_future().share();std::atomic<unsigned> next{0},active{0},peak{0};std::array<Tile,tasks> outputs;std::vector<std::future<void>> threads;
      for(unsigned k=0;k<workers;++k)threads.push_back(std::async(std::launch::async,[&,gate]{gate.wait();for(;;){auto index=next.fetch_add(1);if(index>=tasks)return;auto count=active.fetch_add(1)+1;auto prior=peak.load();while(prior<count&&!peak.compare_exchange_weak(prior,count)){}outputs[index]=node->render_level(rects[index],level);active.fetch_sub(1);}}));
      auto start=Clock::now();signal.set_value();for(auto& thread:threads)thread.get();auto duration=ms(Clock::now()-start);require(active==0&&next>=tasks&&peak<=workers&&peak>=1,"worker state");
      for(unsigned k=0;k<tasks;++k){require(same(outputs[k].bounds,rects[k])&&outputs[k].descriptor==expected[k].descriptor&&outputs[k].rgb.size()==expected[k].rgb.size()&&std::memcmp(outputs[k].rgb.data(),expected[k].rgb.data(),outputs[k].rgb.size()*4)==0,"concurrent tile differs");++comparisons;}
      if(repeat){elapsed.push_back(duration);peaks.push_back(peak.load());++batches;}
     }
     if(!first)report<<',';first=false;report<<"{\"space\":"<<unsigned(space)<<",\"radius\":"<<settings.radius<<",\"epsilon\":"<<settings.epsilon<<",\"mip\":"<<mip<<",\"workers\":"<<workers<<",\"fixed_tasks\":8,\"source_bytes_per_batch\":"<<source_bytes<<",\"max_single_node_logical_bytes\":"<<max_payload<<",\"upper_concurrent_node_logical_bytes\":"<<max_payload*workers<<",\"retained_batch_outputs_bytes\":"<<tasks*128*128*12<<",\"median_batch_ms\":"<<median(elapsed)<<",\"samples_ms\":[";
     for(unsigned i=0;i<elapsed.size();++i){if(i)report<<',';report<<elapsed[i];}report<<"],\"observed_peak_calls\":[";for(unsigned i=0;i<peaks.size();++i){if(i)report<<',';report<<peaks[i];}report<<"]}";report.flush();
    }
    std::cout<<unsigned(space)<<" radius"<<settings.radius<<" mip"<<mip<<" complete\n"<<std::flush;
   }
  }
 }
 PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=0,"process memory counter");wchar_t dll[32768]{};require(GetModuleFileNameW(GetModuleHandleW(L"RawEngine.dll"),dll,32768)>0,"loaded DLL path");
 report<<"],\"complete\":true,\"case_count\":54,\"measured_batches\":"<<batches<<",\"exact_outputs_including_warmup\":"<<comparisons<<",\"peak_working_set_bytes\":"<<memory.PeakWorkingSetSize<<",\"peak_commit_bytes\":"<<memory.PeakPagefileUsage<<",\"private_bytes_at_finish\":"<<memory.PrivateUsage<<",\"loaded_dll\":\""<<std::filesystem::path(dll).generic_string()<<"\"}\n";
 std::cout<<batches<<" fixed-work batches,"<<comparisons<<" exact outputs\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
