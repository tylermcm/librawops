#include "TileScheduler.hpp"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace rawengine {

struct TileScheduler::Impl {
    struct Job {
        std::shared_ptr<const Node> output;
        Rect source_bounds;
        RenderRequest request;
        RenderPriority priority;
        std::shared_ptr<CancellationToken> cancellation;
        std::string group;
        std::uint64_t sequence;
        std::promise<Tile> result;
    };
    struct Compare {
        bool operator()(const std::shared_ptr<Job>& a,
                        const std::shared_ptr<Job>& b) const noexcept {
            if (a->priority != b->priority)
                return static_cast<int>(a->priority) < static_cast<int>(b->priority);
            return a->sequence > b->sequence;
        }
    };

    explicit Impl(std::size_t workers, std::size_t max_pending)
        : max_pending(max_pending) {
        if (!workers || workers > 64 || !max_pending)
            throw std::invalid_argument("scheduler needs 1-64 workers and a pending budget");
        try {
            for (std::size_t i = 0; i < workers; ++i)
                threads.emplace_back([this] { work(); });
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                stopping = true;
            }
            ready.notify_all();
            for (auto& thread : threads) thread.join();
            throw;
        }
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            while (!pending.empty()) {
                try {
                    pending.top()->result.set_exception(
                        std::make_exception_ptr(RenderCancelled()));
                } catch (...) {
                    // Destroying the promise still settles its future.
                }
                pending.pop();
            }
            latest.clear();
        }
        ready.notify_all();
        for (auto& thread : threads) thread.join();
    }

    void work() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping || !pending.empty(); });
                if (stopping && pending.empty()) return;
                job = pending.top(); pending.pop();
            }
            std::optional<Tile> tile;
            std::exception_ptr failure;
            try {
                if (job->cancellation && job->cancellation->is_cancelled())
                    throw RenderCancelled();
                tile = Renderer().render_image(*job->output, job->source_bounds,
                    job->request, job->cancellation.get());
            } catch (...) {
                failure = std::current_exception();
            }
            std::lock_guard lock(mutex);
            if (!failure && job->cancellation && job->cancellation->is_cancelled())
                failure = std::make_exception_ptr(RenderCancelled());
            if (failure) job->result.set_exception(failure);
            else job->result.set_value(std::move(*tile));
            if (!job->group.empty()) {
                auto found = latest.find(job->group);
                if (found != latest.end() && found->second.lock() == job->cancellation)
                    latest.erase(found);
            }
        }
    }

    const std::size_t max_pending;
    std::mutex mutex;
    std::condition_variable ready;
    std::priority_queue<std::shared_ptr<Job>, std::vector<std::shared_ptr<Job>>, Compare> pending;
    std::map<std::string, std::weak_ptr<CancellationToken>> latest;
    std::vector<std::thread> threads;
    std::uint64_t next_sequence = 0;
    bool stopping = false;
};

TileScheduler::TileScheduler(std::size_t workers, std::size_t max_pending)
    : impl_(std::make_unique<Impl>(workers, max_pending)) {}

TileScheduler::~TileScheduler() = default;

std::future<Tile> TileScheduler::submit(
    std::shared_ptr<const Node> output, Rect source_bounds, RenderRequest request,
    RenderPriority priority, std::shared_ptr<CancellationToken> cancellation) {
    return submit_request({}, std::move(output), source_bounds, request,
                          priority, std::move(cancellation));
}

std::future<Tile> TileScheduler::submit(
    std::shared_ptr<const Node> output, Rect source_bounds, Rect viewport,
    RenderPriority priority, std::uint32_t tile_size,
    std::shared_ptr<CancellationToken> cancellation) {
    return submit(std::move(output), source_bounds,
                  RenderRequest{viewport, tile_size, {}}, priority,
                  std::move(cancellation));
}

std::future<Tile> TileScheduler::submit_latest(
    std::string group, std::shared_ptr<const Node> output,
    Rect source_bounds, RenderRequest request, RenderPriority priority) {
    if (group.empty()) throw std::invalid_argument("latest request group is empty");
    return submit_request(std::move(group), std::move(output), source_bounds,
                          request, priority, std::make_shared<CancellationToken>());
}

std::future<Tile> TileScheduler::submit_latest(
    std::string group, std::shared_ptr<const Node> output,
    Rect source_bounds, Rect viewport, RenderPriority priority,
    std::uint32_t tile_size) {
    return submit_latest(std::move(group), std::move(output), source_bounds,
                         RenderRequest{viewport, tile_size, {}}, priority);
}

std::future<Tile> TileScheduler::submit_request(
    std::string group, std::shared_ptr<const Node> output,
    Rect source_bounds, RenderRequest request, RenderPriority priority,
    std::shared_ptr<CancellationToken> cancellation) {
    const Rect viewport = request.viewport;
    const bool native = request.level.mip == 0 &&
        (request.level.quality == RenderQuality::Final ||
         request.level.quality == RenderQuality::Preview);
    const bool reduced = request.level.mip == 1 &&
                         request.level.quality == RenderQuality::Preview &&
                         source_bounds.x == 0 && source_bounds.y == 0;
    const Rect output_bounds = reduced
        ? Rect{0, 0, source_bounds.width / 2 + source_bounds.width % 2,
                   source_bounds.height / 2 + source_bounds.height % 2}
        : source_bounds;
    if (!output || !request.tile_size || (!native && !reduced) ||
        !output->supports_level(request.level) ||
        !source_bounds.width || !source_bounds.height ||
        static_cast<int>(priority) < static_cast<int>(RenderPriority::Background) ||
        static_cast<int>(priority) > static_cast<int>(RenderPriority::Interactive) ||
        static_cast<std::uint64_t>(source_bounds.x) + source_bounds.width >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1 ||
        static_cast<std::uint64_t>(source_bounds.y) + source_bounds.height >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1 ||
        viewport.x < output_bounds.x || viewport.y < output_bounds.y ||
        static_cast<std::uint64_t>(viewport.x) + viewport.width >
            static_cast<std::uint64_t>(output_bounds.x) + output_bounds.width ||
        static_cast<std::uint64_t>(viewport.y) + viewport.height >
            static_cast<std::uint64_t>(output_bounds.y) + output_bounds.height)
        throw std::invalid_argument("invalid scheduled render request");
    auto job = std::make_shared<Impl::Job>();
    job->output = std::move(output);
    job->source_bounds = source_bounds;
    job->request = request;
    job->priority = priority;
    job->cancellation = std::move(cancellation);
    job->group = std::move(group);
    auto result = job->result.get_future();
    {
        std::lock_guard lock(impl_->mutex);
        std::size_t removable = 0;
        if (!job->group.empty()) {
            auto copy = impl_->pending;
            while (!copy.empty()) {
                if (copy.top()->group == job->group) ++removable;
                copy.pop();
            }
        }
        if (impl_->pending.size() - removable >= impl_->max_pending)
            throw std::length_error("scheduler pending request budget is full");
        if (!job->group.empty()) {
            if (auto found = impl_->latest.find(job->group); found != impl_->latest.end())
                if (auto previous = found->second.lock()) previous->cancel();
            decltype(impl_->pending) kept;
            while (!impl_->pending.empty()) {
                auto queued = impl_->pending.top(); impl_->pending.pop();
                if (queued->group == job->group) {
                    queued->cancellation->cancel();
                    queued->result.set_exception(std::make_exception_ptr(RenderCancelled()));
                } else kept.push(std::move(queued));
            }
            impl_->pending = std::move(kept);
            impl_->latest[job->group] = job->cancellation;
        }
        job->sequence = impl_->next_sequence++;
        impl_->pending.push(std::move(job));
    }
    impl_->ready.notify_one();
    return result;
}

} // namespace rawengine
