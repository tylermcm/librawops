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

// These names describe what the prototype actually knows. CameraNative is not
// an ICC profile; the camera-to-working-space transform has not been added.
enum class PixelFormat { RGBFloat32 };
enum class PixelDomain { CameraLinearRGB, ToneMappedUnmanagedRGB, BoundedUnmanagedRGB };
enum class ColorPrimaries { CameraNative };
enum class WhitePoint { Unspecified };
enum class TransferFunction { Linear, CustomTone };
enum class ReferenceState { CameraReferred, Unspecified };
enum class AlphaMode { None };

struct ImageDescriptor {
    PixelFormat format = PixelFormat::RGBFloat32;
    PixelDomain domain = PixelDomain::CameraLinearRGB;
    ColorPrimaries primaries = ColorPrimaries::CameraNative;
    WhitePoint white_point = WhitePoint::Unspecified;
    TransferFunction transfer = TransferFunction::Linear;
    ReferenceState reference = ReferenceState::CameraReferred;
    AlphaMode alpha = AlphaMode::None;

    static constexpr ImageDescriptor camera_linear() noexcept { return {}; }
    static constexpr ImageDescriptor tone_mapped() noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::ToneMappedUnmanagedRGB,
                ColorPrimaries::CameraNative, WhitePoint::Unspecified,
                TransferFunction::CustomTone, ReferenceState::Unspecified, AlphaMode::None};
    }
    static constexpr ImageDescriptor bounded_output() noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::BoundedUnmanagedRGB,
                ColorPrimaries::CameraNative, WhitePoint::Unspecified,
                TransferFunction::CustomTone, ReferenceState::Unspecified, AlphaMode::None};
    }
    bool operator==(const ImageDescriptor&) const = default;
};

struct Tile {
    Rect bounds;
    std::vector<float> rgb; // interleaved float32 RGB, row-major; 3 floats/pixel
    ImageDescriptor descriptor = ImageDescriptor::camera_linear();
};

class RAWENGINE_API Node {
public:
    virtual ~Node() = default;
    virtual Tile render(Rect bounds) const = 0;
    virtual ImageDescriptor output_descriptor() const noexcept = 0;
};

class RAWENGINE_API RawUnpackNode final : public Node {
public:
    explicit RawUnpackNode(RawImage image);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
private:
    RawImage image_;
};

class RAWENGINE_API WhiteBalanceNode final : public Node {
public:
    WhiteBalanceNode(std::shared_ptr<const Node> input, float red, float green, float blue);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
private:
    std::shared_ptr<const Node> input_;
    float gains_[3];
};

class RAWENGINE_API ExposureNode final : public Node {
public:
    ExposureNode(std::shared_ptr<const Node> input, float stops);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
private:
    std::shared_ptr<const Node> input_;
    float multiplier_;
};

class RAWENGINE_API ToneCurveNode final : public Node {
public:
    ToneCurveNode(std::shared_ptr<const Node> input, float shoulder, float gamma);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::tone_mapped();
    }
private:
    std::shared_ptr<const Node> input_;
    float shoulder_, inverse_gamma_;
};

// The only stage that clips signed/over-range values for the current public
// render path. It does not apply a display ICC profile or a standard transfer.
class RAWENGINE_API OutputClipNode final : public Node {
public:
    explicit OutputClipNode(std::shared_ptr<const Node> input);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::bounded_output();
    }
private:
    std::shared_ptr<const Node> input_;
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
    // Materializes only the requested viewport, retaining its descriptor.
    Tile render_image(const ImageGraph& graph, Rect viewport,
                      std::uint32_t tile_size = 256) const;
    // Legacy convenience API for callers that only need interleaved floats.
    std::vector<float> render_roi(const ImageGraph& graph, Rect viewport,
                                  std::uint32_t tile_size = 256) const;
};

} // namespace rawengine
