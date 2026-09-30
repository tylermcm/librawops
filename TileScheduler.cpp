#include "TileScheduler.hpp"

#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace rawengine {

struct TileScheduler::Impl {
    struct Job {
        std::shared_ptr<const Node> output;
        Rect source_bounds, viewport;
        RenderPriority priority;
        std::uint32_t tile_size;
        std::shared_ptr<CancellationToken> cancellation;
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
            try {
                if (job->cancellation && job->cancellation->is_cancelled())
                    throw RenderCancelled();
                Tile tile = Renderer().render_image(*job->output, job->source_bounds,
                    job->viewport, job->tile_size, job->cancellation.get());
                if (job->cancellation && job->cancellation->is_cancelled())
                    throw RenderCancelled();
                job->result.set_value(std::move(tile));
            } catch (...) {
                job->result.set_exception(std::current_exception());
            }
        }
    }

    const std::size_t max_pending;
    std::mutex mutex;
    std::condition_variable ready;
    std::priority_queue<std::shared_ptr<Job>, std::vector<std::shared_ptr<Job>>, Compare> pending;
    std::vector<std::thread> threads;
    std::uint64_t next_sequence = 0;
    bool stopping = false;
};

TileScheduler::TileScheduler(std::size_t workers, std::size_t max_pending)
    : impl_(std::make_unique<Impl>(workers, max_pending)) {}

TileScheduler::~TileScheduler() = default;

std::future<Tile> TileScheduler::submit(
    std::shared_ptr<const Node> output, Rect source_bounds, Rect viewport,
    RenderPriority priority, std::uint32_t tile_size,
    std::shared_ptr<CancellationToken> cancellation) {
    if (!output || !tile_size ||
        !source_bounds.width || !source_bounds.height ||
        static_cast<int>(priority) < static_cast<int>(RenderPriority::Background) ||
        static_cast<int>(priority) > static_cast<int>(RenderPriority::Interactive) ||
        static_cast<std::uint64_t>(source_bounds.x) + source_bounds.width >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1 ||
        static_cast<std::uint64_t>(source_bounds.y) + source_bounds.height >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1 ||
        viewport.x < source_bounds.x || viewport.y < source_bounds.y ||
        static_cast<std::uint64_t>(viewport.x) + viewport.width >
            static_cast<std::uint64_t>(source_bounds.x) + source_bounds.width ||
        static_cast<std::uint64_t>(viewport.y) + viewport.height >
            static_cast<std::uint64_t>(source_bounds.y) + source_bounds.height)
        throw std::invalid_argument("invalid scheduled render request");
    auto job = std::make_shared<Impl::Job>();
    job->output = std::move(output);
    job->source_bounds = source_bounds;
    job->viewport = viewport;
    job->priority = priority;
    job->tile_size = tile_size;
    job->cancellation = std::move(cancellation);
    auto result = job->result.get_future();
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->pending.size() >= impl_->max_pending)
            throw std::length_error("scheduler pending request budget is full");
        job->sequence = impl_->next_sequence++;
        impl_->pending.push(std::move(job));
    }
    impl_->ready.notify_one();
    return result;
}

} // namespace rawengine
