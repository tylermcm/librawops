#pragma once

#include "RawEngine.hpp"

#include <future>
#include <memory>
#include <string>

namespace rawengine {

// Queued requests run in priority order, FIFO within one priority. Running
// requests are not preempted. The node handle retains the immutable graph.
enum class RenderPriority { Background = 0, Normal = 1, Interactive = 2 };

class RAWENGINE_API TileScheduler final {
public:
    TileScheduler(std::size_t workers, std::size_t max_pending);
    ~TileScheduler();
    TileScheduler(const TileScheduler&) = delete;
    TileScheduler& operator=(const TileScheduler&) = delete;

    // Throws length_error when the pending queue is full. Cancellation is
    // shared with the caller and checked before/between tiles; an aborted
    // future raises RenderCancelled. Destruction drops pending requests and
    // waits for running requests to finish.
    std::future<Tile> submit(std::shared_ptr<const Node> output,
                             Rect source_bounds, RenderRequest request,
                             RenderPriority priority = RenderPriority::Normal,
                             std::shared_ptr<CancellationToken> cancellation = nullptr);
    std::future<Tile> submit(std::shared_ptr<const Node> output,
                             Rect source_bounds, Rect viewport,
                             RenderPriority priority = RenderPriority::Normal,
                             std::uint32_t tile_size = 256,
                             std::shared_ptr<CancellationToken> cancellation = nullptr);

    // Only the newest request in a nonempty group remains actionable. A new
    // request removes older queued jobs in that group and cancels an active
    // one at its next tile boundary. Its token is managed by the scheduler.
    std::future<Tile> submit_latest(std::string group,
                                    std::shared_ptr<const Node> output,
                                    Rect source_bounds, RenderRequest request,
                                    RenderPriority priority = RenderPriority::Interactive);
    std::future<Tile> submit_latest(std::string group,
                                    std::shared_ptr<const Node> output,
                                    Rect source_bounds, Rect viewport,
                                    RenderPriority priority = RenderPriority::Interactive,
                                    std::uint32_t tile_size = 256);
private:
    std::future<Tile> submit_request(std::string group,
                                     std::shared_ptr<const Node> output,
                                     Rect source_bounds, RenderRequest request,
                                     RenderPriority priority,
                                     std::shared_ptr<CancellationToken> cancellation);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rawengine
