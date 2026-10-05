#include "TileScheduler.hpp"
#include "reference/mask_graph_numeric_v1.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace rawengine;
namespace {
void require(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
template<class Exception=std::invalid_argument,class Callback> void rejects(Callback action) {
    try {action();} catch (const Exception&) {return;}
    throw std::runtime_error("expected rejection did not occur");
}
template<class Values> std::vector<float> decode(const Values& words) {
    std::vector<float> values;for (auto word:words) values.push_back(std::bit_cast<float>(word));return values;
}
bool exact(const std::vector<float>& a,const std::vector<float>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](float x,float y) {
        return std::bit_cast<std::uint32_t>(x)==std::bit_cast<std::uint32_t>(y);
    });
}
std::shared_ptr<const CoverageNode> source(Rect bounds={0,0,7,5}) {
    return std::make_shared<CoverageRasterNode>(CoverageImage({bounds,0},decode(mask_graph_reference::mask_a)));
}
std::shared_ptr<const Node> rgb_source() {
    return std::make_shared<RasterSourceNode>(RasterImage({7,5,0,WorkingSpace::LinearProPhotoD50},
        decode(mask_graph_reference::base_rgb)));
}
void streaming() {
    auto inverse=std::make_shared<CoverageInvertNode>(source());
    auto second=std::make_shared<CoverageRasterNode>(CoverageImage({{0,0,7,5},0},decode(mask_graph_reference::mask_b)));
    CoverageCombineNode node(inverse,second,MaskCombineMode::Intersect);CoverageRenderer renderer;
    for (RenderLevel level:{RenderLevel{},RenderLevel{0,RenderQuality::Preview},RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
        const auto full=node.output_bounds(level);const auto expected=level.mip==0?decode(mask_graph_reference::coverage_mip0):
            level.mip==1?decode(mask_graph_reference::coverage_mip1):decode(mask_graph_reference::coverage_mip2);
        for (unsigned size:{1u,2u,8u}) {
            auto image=renderer.render_image(node,RenderRequest{full,size,level});require(exact(image.coverage,expected),"scalar assembly differs from frozen bits");
            std::vector<float> assembled(expected.size(),-1);std::uint64_t last=0;bool first=true;
            renderer.render_tiles(node,RenderRequest{full,size,level},[&](const auto& tile) {
                const auto order=static_cast<std::uint64_t>(tile.bounds.y)*full.width+tile.bounds.x;
                require(first || order>last,"scalar stream is not row major");last=order;first=false;
                for (unsigned y=0;y<tile.bounds.height;++y) for (unsigned x=0;x<tile.bounds.width;++x)
                    assembled[(tile.bounds.y+y)*full.width+tile.bounds.x+x]=tile.coverage[y*tile.bounds.width+x];
            });
            require(exact(assembled,expected),"scalar stream differs from frozen bits");
            for (unsigned y=0;y<full.height;++y) for (unsigned x=0;x<full.width;++x)
                require(renderer.render_image(node,RenderRequest{{x,y,1,1},size,level}).coverage[0]==expected[y*full.width+x],"scalar ROI changed output");
        }
    }
    auto shifted=source({9,11,7,5});const auto expected=decode(mask_graph_reference::mask_a);
    require(renderer.render_image(*shifted,RenderRequest{{9,11,7,5},2,{}}).coverage==expected,"scalar native origin lost");
    unsigned callbacks=0;renderer.render_tiles(node,RenderRequest{{7,5,0,0},1,{}},[&](const auto&) {++callbacks;});
    require(!callbacks && renderer.render_image(node,RenderRequest{{7,5,0,0},1,{}}).coverage.empty(),"empty contained scalar ROI emitted data");
    rejects([&] {renderer.render_image(node,RenderRequest{{0,0,7,5},0,{}});});
    rejects([&] {renderer.render_tiles(node,RenderRequest{{0,0,7,5},1,{}},{});});
    rejects([&] {renderer.render_image(node,RenderRequest{{7,5,1,1},1,{}});});
    rejects([&] {renderer.render_image(node,RenderRequest{{0,0,1,1},1,{1,RenderQuality::Final}});});
    CancellationToken cancelled;cancelled.cancel();
    rejects<RenderCancelled>([&] {renderer.render_image(node,RenderRequest{{0,0,7,5},1,{}},&cancelled);});
    for (unsigned size:{1u,8u}) {
        CancellationToken token;unsigned visits=0;
        rejects<RenderCancelled>([&] {renderer.render_tiles(node,RenderRequest{{0,0,7,5},size,{}},[&](const auto&) {++visits;token.cancel();},&token);});
        require(visits==1,"scalar callback cancellation did not stop after first tile");
    }
    rejects<std::runtime_error>([&] {renderer.render_tiles(node,RenderRequest{{0,0,7,5},1,{}},[](const auto&) {throw std::runtime_error("callback");});});
    require(renderer.render_image(node,RenderRequest{{0,0,7,5},2,{}}).coverage==decode(mask_graph_reference::coverage_mip0),"callback failure poisoned later render");
}

class BadCoverage final:public CoverageNode {
public:
    explicit BadCoverage(int damage):damage_(damage) {}
    CoverageTile render_level(Rect r,RenderLevel) const override {
        CoverageTile tile{r,std::vector<float>(static_cast<std::size_t>(r.width)*r.height,0.5f)};
        if (damage_==0) ++tile.bounds.x;
        if (damage_==1) tile.coverage.pop_back();
        if (damage_==2) tile.coverage.back()=std::numeric_limits<float>::quiet_NaN();
        if (damage_==3) tile.coverage.back()=-0.1f;
        return tile;
    }
    bool supports_level(RenderLevel level) const noexcept override {return level.mip==0;}
    Rect native_bounds() const noexcept override {return {0,0,7,5};}
private:int damage_;
};
void actual_guards() {
    for (int damage=0;damage<4;++damage) {
        BadCoverage bad(damage);unsigned calls=0;
        rejects<std::exception>([&] {CoverageRenderer{}.render_tiles(bad,RenderRequest{{0,0,1,1},1,{}},[&](const auto&) {++calls;});});
        require(!calls,"malformed actual scalar tile reached callback");
        rejects<std::exception>([&] {CoverageRenderer{}.render_image(bad,RenderRequest{{0,0,1,1},1,{}});});
    }
}
class DeclaredCoverage final:public CoverageNode {
public:
    explicit DeclaredCoverage(Rect extent):extent_(extent) {}
    CoverageTile render_level(Rect,RenderLevel) const override {throw std::logic_error("request guard failed before rendering");}
    bool supports_level(RenderLevel) const noexcept override {return true;}
    Rect native_bounds() const noexcept override {return extent_;}
    Rect output_bounds(RenderLevel) const override {return extent_;}
private:Rect extent_;
};
void declared_guards() {
    constexpr auto maximum=std::numeric_limits<std::uint32_t>::max();
    DeclaredCoverage overflow({maximum,0,2,1});
    rejects([&] {CoverageRenderer{}.render_image(overflow,RenderRequest{{maximum,0,1,1},1,{}});});
    DeclaredCoverage huge({0,0,maximum,maximum});
    rejects<std::length_error>([&] {CoverageRenderer{}.render_image(huge,RenderRequest{{0,0,maximum,maximum},256,{}});});
    DeclaredCoverage unsupported({0,0,1,1});
    rejects([&] {CoverageRenderer{}.render_image(unsupported,RenderRequest{{0,0,1,1},1,{3,RenderQuality::Preview}});});
}

struct Gate {
    std::promise<void> entered,release;
    std::shared_future<void> released=release.get_future().share();
    std::atomic<bool> first{true},opened{false};
    void wait() {if (first.exchange(false)) {entered.set_value();released.wait();}}
    void open() {if (!opened.exchange(true)) release.set_value();}
};
struct ReleaseOnExit {std::shared_ptr<Gate> gate;~ReleaseOnExit() {gate->open();}};
void await(std::future<void>& entered) {
    require(entered.wait_for(std::chrono::seconds(10))==std::future_status::ready,"worker did not reach promise gate");
}
struct Log {std::mutex mutex;std::vector<int> values;void add(int value) {std::lock_guard lock(mutex);values.push_back(value);}};
class ControlledCoverage final:public CoverageNode {
public:
    ControlledCoverage(std::shared_ptr<Gate> gate={},std::shared_ptr<Log> log={},int number=0,bool fault=false)
        :gate_(std::move(gate)),log_(std::move(log)),number_(number),fault_(fault) {}
    CoverageTile render_level(Rect r,RenderLevel level) const override {
        if (gate_) gate_->wait();if (fault_.exchange(false)) throw std::bad_alloc();
        if (log_) log_->add(number_);return input_->render_level(r,level);
    }
    bool supports_level(RenderLevel level) const noexcept override {return input_->supports_level(level);}
    Rect native_bounds() const noexcept override {return input_->native_bounds();}
private:
    std::shared_ptr<const CoverageNode> input_=source();std::shared_ptr<Gate> gate_;std::shared_ptr<Log> log_;
    int number_;mutable std::atomic<bool> fault_;
};
class LoggedRgb final:public Node {
public:
    LoggedRgb(std::shared_ptr<Log> log,int number):log_(std::move(log)),number_(number) {}
    Tile render(Rect r) const override {log_->add(number_);return input_->render(r);}
    ImageDescriptor output_descriptor() const noexcept override {return input_->output_descriptor();}
private:std::shared_ptr<const Node> input_=rgb_source();std::shared_ptr<Log> log_;int number_;
};
class GateRgb final:public Node {
public:
    explicit GateRgb(std::shared_ptr<Gate> gate):gate_(std::move(gate)) {}
    Tile render(Rect r) const override {gate_->wait();return input_->render(r);}
    ImageDescriptor output_descriptor() const noexcept override {return input_->output_descriptor();}
private:std::shared_ptr<const Node> input_=rgb_source();std::shared_ptr<Gate> gate_;
};
const RenderRequest full{{0,0,7,5},8,{}};
void queue_order_and_budget() {
    auto gate=std::make_shared<Gate>();auto entered=gate->entered.get_future();auto log=std::make_shared<Log>();
    TileScheduler scheduler(1,3);ReleaseOnExit release{gate};
    auto active=scheduler.submit(std::make_shared<ControlledCoverage>(gate),full);await(entered);
    auto normal=scheduler.submit(std::make_shared<ControlledCoverage>(nullptr,log,1),full,RenderPriority::Normal);
    auto rgb=scheduler.submit(std::make_shared<LoggedRgb>(log,2),Rect{0,0,7,5},full,RenderPriority::Interactive);
    auto scalar=scheduler.submit(std::make_shared<ControlledCoverage>(nullptr,log,3),full,RenderPriority::Interactive);
    rejects<std::length_error>([&] {scheduler.submit(source(),full);});
    rejects<std::length_error>([&] {scheduler.submit(rgb_source(),Rect{0,0,7,5},full);});
    gate->open();active.get();normal.get();rgb.get();scalar.get();
    require(log->values==std::vector<int>({2,3,1}),"mixed queue priority/FIFO changed");
}
void groups_and_recovery() {
    auto gate=std::make_shared<Gate>();auto entered=gate->entered.get_future();
    TileScheduler scheduler(1,2);ReleaseOnExit release{gate};
    auto active=scheduler.submit_latest("viewport",std::make_shared<ControlledCoverage>(gate),full);await(entered);
    auto token=std::make_shared<CancellationToken>();
    auto rgb=scheduler.submit_latest("viewport",rgb_source(),Rect{0,0,7,5},full,RenderPriority::Interactive,token);
    rejects([&] {scheduler.submit_latest("viewport",source(),RenderRequest{{0,0,7,5},0,{}});});
    require(!token->is_cancelled(),"invalid scalar replacement cancelled accepted RGB request");
    auto newest=scheduler.submit_latest("viewport",source(),full);
    require(token->is_cancelled(),"scalar newest did not cancel queued RGB in same group");
    rejects<RenderCancelled>([&] {rgb.get();});
    auto unrelated=scheduler.submit(source(),full);
    auto newest_rgb=scheduler.submit_latest("viewport",rgb_source(),Rect{0,0,7,5},full);
    rejects<RenderCancelled>([&] {newest.get();});
    auto final=scheduler.submit_latest("viewport",source(),full);
    rejects<RenderCancelled>([&] {newest_rgb.get();});gate->open();
    rejects<RenderCancelled>([&] {active.get();});
    require(final.get().coverage==decode(mask_graph_reference::mask_a),"newest scalar did not recover");
    unrelated.get();
    rejects([&] {scheduler.submit_latest("",source(),full);});
    rejects([&] {scheduler.submit(std::shared_ptr<const CoverageNode>{},full);});
    rejects([&] {scheduler.submit(source(),full,static_cast<RenderPriority>(99));});
    auto fault_gate=std::make_shared<Gate>();auto fault_entered=fault_gate->entered.get_future();ReleaseOnExit fault_release{fault_gate};
    auto fault=std::make_shared<ControlledCoverage>(fault_gate,nullptr,0,true);
    auto failed=scheduler.submit(fault,full);await(fault_entered);
    auto queued=scheduler.submit(fault,full);fault_gate->open();
    rejects<std::bad_alloc>([&] {failed.get();});
    require(queued.get().coverage==decode(mask_graph_reference::mask_a),"queued scalar job failed after source fault");
}
void active_rgb_supersession() {
    auto gate=std::make_shared<Gate>();auto entered=gate->entered.get_future();
    TileScheduler scheduler(1,1);ReleaseOnExit release{gate};
    auto active=scheduler.submit_latest("view",std::make_shared<GateRgb>(gate),Rect{0,0,7,5},full);await(entered);
    auto newest=scheduler.submit_latest("view",source(),full);gate->open();
    rejects<RenderCancelled>([&] {active.get();});
    require(newest.get().coverage==decode(mask_graph_reference::mask_a),"scalar could not supersede active RGB job");
}
void destruction() {
    auto gate=std::make_shared<Gate>();auto entered=gate->entered.get_future();
    auto scheduler=std::make_unique<TileScheduler>(1,2);ReleaseOnExit release{gate};
    auto active=scheduler->submit(std::make_shared<ControlledCoverage>(gate),full);await(entered);
    auto pending=scheduler->submit(source(),full);
    auto destroyed=std::async(std::launch::async,[owned=std::move(scheduler)]() mutable {owned.reset();});
    require(pending.wait_for(std::chrono::seconds(10))==std::future_status::ready,"scheduler destruction did not settle pending scalar job");
    rejects<RenderCancelled>([&] {pending.get();});gate->open();active.get();destroyed.get();
}
} // namespace
int main() {
    try {streaming();actual_guards();declared_guards();queue_order_and_budget();groups_and_recovery();active_rgb_supersession();destruction();
        std::cout<<"Frozen scalar delivery/ROI/stream/guards/cancellation and promise-gated mixed queue policies passed\n";return 0;}
    catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
