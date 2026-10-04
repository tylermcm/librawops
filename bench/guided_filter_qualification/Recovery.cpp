#include "SpatialOps.hpp"
#include "TileScheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace rawengine;

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

enum class Fault { Allocation, Backend, Storage, Descriptor, Nonfinite };

// Mutable one-shot fault state belongs only to this diagnostic source. The
// production raster and guided-filter settings remain immutable and shared.
class FaultSource final : public Node {
public:
    FaultSource(std::shared_ptr<const Node> source, Fault fault, unsigned at_call,
                std::shared_future<void> release)
        : source_(std::move(source)), fault_(fault), at_call_(at_call),
          release_(std::move(release)) {}

    std::future<void> entered() { return entered_.get_future(); }
    Tile render(Rect bounds) const override { return render_level(bounds, {}); }
    bool supports_level(RenderLevel level) const noexcept override {
        return source_->supports_level(level);
    }
    ImageDescriptor output_descriptor() const noexcept override {
        return source_->output_descriptor();
    }
    Tile render_level(Rect bounds, RenderLevel level) const override {
        const auto call = calls_.fetch_add(1) + 1;
        if (call != at_call_) return source_->render_level(bounds, level);
        entered_.set_value();
        require(release_.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
                "fault release timed out");
        if (fault_ == Fault::Allocation) throw std::bad_alloc();
        if (fault_ == Fault::Backend) throw std::runtime_error("injected source failure");
        auto value = source_->render_level(bounds, level);
        if (fault_ == Fault::Storage) value.rgb.pop_back();
        if (fault_ == Fault::Descriptor) {
            value.descriptor = ImageDescriptor::scene_linear(
                value.descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50)
                    ? WorkingSpace::LinearRec2020D65 : WorkingSpace::LinearProPhotoD50);
        }
        if (fault_ == Fault::Nonfinite) value.rgb[0] = std::numeric_limits<float>::quiet_NaN();
        return value;
    }

private:
    std::shared_ptr<const Node> source_;
    Fault fault_;
    unsigned at_call_;
    std::shared_future<void> release_;
    mutable std::promise<void> entered_;
    mutable std::atomic<unsigned> calls_{0};
};

bool same(const Tile& a, const Tile& b) {
    return a.bounds.x == b.bounds.x && a.bounds.y == b.bounds.y &&
           a.bounds.width == b.bounds.width && a.bounds.height == b.bounds.height &&
           a.descriptor == b.descriptor && a.rgb.size() == b.rgb.size() &&
           std::memcmp(a.rgb.data(), b.rgb.data(), a.rgb.size() * sizeof(float)) == 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2 && !std::filesystem::exists(argv[1]), "fresh report path required");
        constexpr unsigned width = 65, height = 49;
        unsigned failures = 0, recovered = 0, repeated = 0;
        for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65}) {
            std::vector<float> pixels(std::size_t(width) * height * 3);
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                    for (unsigned c = 0; c < 3; ++c)
                        pixels[(std::size_t(y) * width + x) * 3 + c] =
                            float(int((19 * x + 37 * y + 83 * c) % 257) - 64) / 128;
            auto source = std::make_shared<RasterSourceNode>(
                RasterImage({width, height, 0, space}, std::move(pixels)));
            for (unsigned radius : {0u, 3u, 8u}) {
                WorkingYGuidedFilterSettings settings{radius, radius == 3 ? 0x1p-12 : 0x1p-24};
                auto healthy = std::make_shared<WorkingYGuidedFilterNode>(
                    source, Rect{0, 0, width, height}, settings);
                for (auto level : {RenderLevel{0, RenderQuality::Final},
                                   RenderLevel{0, RenderQuality::Preview},
                                   RenderLevel{1, RenderQuality::Preview},
                                   RenderLevel{2, RenderQuality::Preview}}) {
                    const unsigned scale = 1u << level.mip;
                    Rect image{0, 0, (width + scale - 1) / scale, (height + scale - 1) / scale};
                    const auto expected = healthy->render_level(image, level);
                    for (auto fault : {Fault::Allocation, Fault::Backend, Fault::Storage,
                                       Fault::Descriptor, Fault::Nonfinite}) {
                        for (unsigned at_call : {1u, 2u}) {
                            TileScheduler scheduler(1, 2);
                            std::promise<void> release;
                            auto failing_source = std::make_shared<FaultSource>(
                                source, fault, at_call, release.get_future().share());
                            auto entered = failing_source->entered();
                            auto node = std::make_shared<WorkingYGuidedFilterNode>(
                                failing_source, Rect{0, 0, width, height}, settings);
                            auto failed = scheduler.submit(node, {0, 0, width, height},
                                                           RenderRequest{image, 4, level});
                            const bool reached = entered.wait_for(std::chrono::seconds(10)) ==
                                                 std::future_status::ready;
                            if (!reached) release.set_value();
                            require(reached, "fault boundary not reached");
                            // The sole worker is blocked at the fault boundary,
                            // guaranteeing this healthy request is already queued.
                            auto recovery = scheduler.submit(node, {0, 0, width, height},
                                                             RenderRequest{image, 8, level});
                            release.set_value();
                            bool rejected = false;
                            try { (void)failed.get(); }
                            catch (const std::bad_alloc&) {
                                rejected = fault == Fault::Allocation;
                            }
                            catch (const std::invalid_argument&) {
                                rejected = fault == Fault::Storage || fault == Fault::Descriptor ||
                                           fault == Fault::Nonfinite;
                            }
                            catch (const std::runtime_error& error) {
                                rejected = fault == Fault::Backend &&
                                           std::string(error.what()) == "injected source failure";
                            }
                            require(rejected, "failure type or completion was lost");
                            ++failures;
                            require(same(recovery.get(), expected), "queued recovery differs");
                            ++recovered;
                            auto later = scheduler.submit(node, {0, 0, width, height},
                                                          RenderRequest{image, 16, level});
                            require(same(later.get(), expected), "later recovery differs");
                            ++repeated;
                        }
                    }
                }
            }
        }
        std::ofstream report(argv[1]);
        report << "{\"complete\":true,\"failure_cases\":" << failures
               << ",\"queued_exact_recoveries\":" << recovered
               << ",\"later_exact_recoveries\":" << repeated
               << ",\"allocation_exceptions_injected_at_source_boundary\":48"
               << ",\"actual_allocator_failures_injected\":0";
#ifdef _WIN32
        wchar_t dll[32768]{};
        require(GetModuleFileNameW(GetModuleHandleW(L"RawEngine.dll"), dll, 32768) > 0,
                "loaded DLL path unavailable");
        report << ",\"loaded_dll\":\"" << std::filesystem::path(dll).generic_string() << "\"";
#endif
        report << "}\n";
        std::cout << failures << " failures," << recovered << " queued recoveries,"
                  << repeated << " later recoveries\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
