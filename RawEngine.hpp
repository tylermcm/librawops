#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#if defined(_WIN32) && defined(RAWENGINE_BUILDING)
#  define RAWENGINE_API __declspec(dllexport)
#elif defined(_WIN32)
#  define RAWENGINE_API __declspec(dllimport)
#else
#  define RAWENGINE_API
#endif

namespace rawengine {

enum class BayerPattern { RGGB, BGGR, GRBG, GBRG };

struct Rect {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
};

// Owns the uncompressed, row-major Bayer samples. Metadata may come from any
// RAW decoder; this library does not link to one.
struct RAWENGINE_API RawImage {
    std::uint32_t width = 0, height = 0;
    std::uint16_t black_level = 0, white_level = 65535;
    BayerPattern pattern = BayerPattern::RGGB;
    std::shared_ptr<const std::vector<std::uint16_t>> bayer;

    RawImage(std::uint32_t width, std::uint32_t height,
             std::vector<std::uint16_t> samples,
             BayerPattern pattern = BayerPattern::RGGB,
             std::uint16_t black_level = 0,
             std::uint16_t white_level = 65535);
};

// Immutable editing recipe. A new graph can be made cheaply from the same RAW.
struct GraphRecipe {
    float red_gain = 1.0f, green_gain = 1.0f, blue_gain = 1.0f;
    float exposure_stops = 0.0f;
    float tone_shoulder = 0.25f;
    float tone_gamma = 1.0f;
};

// This prototype has no camera-to-working-space transform or ICC output
// transform yet. Do not treat tone-mapped values as linear or color-managed.
enum class PixelDomain { CameraLinearRGB, ToneMappedUnmanagedRGB };

struct Tile {
    Rect bounds;
    std::vector<float> rgb; // interleaved float32 RGB, row-major; 3 floats/pixel
    PixelDomain domain = PixelDomain::CameraLinearRGB;
};

class RAWENGINE_API Node {
public:
    virtual ~Node() = default;
    virtual Tile render(Rect bounds) const = 0;
    virtual PixelDomain output_domain() const noexcept = 0;
};

class RAWENGINE_API RawUnpackNode final : public Node {
public:
    explicit RawUnpackNode(RawImage image);
    Tile render(Rect bounds) const override;
    PixelDomain output_domain() const noexcept override { return PixelDomain::CameraLinearRGB; }
private:
    RawImage image_;
};

class RAWENGINE_API WhiteBalanceNode final : public Node {
public:
    WhiteBalanceNode(std::shared_ptr<const Node> input, float red, float green, float blue);
    Tile render(Rect bounds) const override;
    PixelDomain output_domain() const noexcept override { return PixelDomain::CameraLinearRGB; }
private:
    std::shared_ptr<const Node> input_;
    float gains_[3];
};

class RAWENGINE_API ExposureNode final : public Node {
public:
    ExposureNode(std::shared_ptr<const Node> input, float stops);
    Tile render(Rect bounds) const override;
    PixelDomain output_domain() const noexcept override { return PixelDomain::CameraLinearRGB; }
private:
    std::shared_ptr<const Node> input_;
    float multiplier_;
};

class RAWENGINE_API ToneCurveNode final : public Node {
public:
    ToneCurveNode(std::shared_ptr<const Node> input, float shoulder, float gamma);
    Tile render(Rect bounds) const override;
    PixelDomain output_domain() const noexcept override { return PixelDomain::ToneMappedUnmanagedRGB; }
private:
    std::shared_ptr<const Node> input_;
    float shoulder_, inverse_gamma_;
};

class RAWENGINE_API ImageGraph final {
public:
    ImageGraph(RawImage image, GraphRecipe recipe = {});
    const RawImage& image() const noexcept { return image_; }
    const GraphRecipe& recipe() const noexcept { return recipe_; }
    const Node& output() const noexcept { return *output_; }
private:
    RawImage image_;
    GraphRecipe recipe_;
    std::shared_ptr<const Node> output_;
};

class RAWENGINE_API Renderer final {
public:
    // The callback receives one temporary tile at a time. Copy data from it
    // before returning if it must outlive the callback.
    using TileCallback = std::function<void(const Tile&)>;
    void render_tiles(const ImageGraph& graph, Rect viewport,
                      const TileCallback& callback,
                      std::uint32_t tile_size = 256) const;
    // Convenience API: allocates only the requested viewport's output.
    std::vector<float> render_roi(const ImageGraph& graph, Rect viewport,
                                  std::uint32_t tile_size = 256) const;
};

} // namespace rawengine
