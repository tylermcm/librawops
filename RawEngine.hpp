#pragma once

#include <array>
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

// Site arrays are in pattern-relative row-major 2x2 order. The phase shifts
// that pattern over sensor coordinates; (0,0) indexes phase_x,phase_y.
struct RawMetadata {
    std::uint32_t width = 0, height = 0;
    std::uint32_t row_stride_samples = 0; // 0 means tightly packed width.
    BayerPattern pattern = BayerPattern::RGGB;
    std::uint8_t cfa_phase_x = 0, cfa_phase_y = 0;
    Rect active_area; // all zero means the full sensor; coordinates remain sensor-relative.
    std::array<std::uint16_t, 4> black_levels{0, 0, 0, 0};
    std::array<std::uint16_t, 4> white_levels{65535, 65535, 65535, 65535};
};

// Owns decoded, uncompressed uint16 Bayer samples. Construction validates and
// normalizes metadata; copies of RawImage share immutable sample storage.
class RAWENGINE_API RawImage {
public:
    RawImage(RawMetadata metadata, std::vector<std::uint16_t> samples);
    // Compatibility constructor for tightly packed, uniform-level Bayer data.
    RawImage(std::uint32_t width, std::uint32_t height,
             std::vector<std::uint16_t> samples,
             BayerPattern pattern = BayerPattern::RGGB,
             std::uint16_t black_level = 0,
             std::uint16_t white_level = 65535);
    const RawMetadata& metadata() const noexcept { return metadata_; }
    const std::vector<std::uint16_t>& samples() const noexcept { return *bayer_; }
    std::uint32_t width() const noexcept { return metadata_.width; }
    std::uint32_t height() const noexcept { return metadata_.height; }
private:
    RawMetadata metadata_;
    std::shared_ptr<const std::vector<std::uint16_t>> bayer_;
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
