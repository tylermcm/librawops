#pragma once

#include "RawEngine.hpp"

#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace rawengine {

// JSON-compatible typed parameter tree. Object keys are ordered so the edit
// manifest has one deterministic byte representation for a given state.
struct EditValue {
    using Array = std::vector<EditValue>;
    using Object = std::map<std::string, EditValue>;
    std::variant<std::nullptr_t, bool, std::int64_t, double,
                 std::string, Array, Object> data = nullptr;
    bool operator==(const EditValue&) const = default;
};

enum class EditSourceKind { DecodedBayerU16, SceneLinearRasterF32, IccRasterU16 };
enum class EditDomain { CameraLinear, SceneLinearProPhotoD50,
                        SceneLinearRec2020D65, DisplayLinearSrgb,
                        DisplayEncodedSrgb, DisplayEncodedIcc, UnmanagedBounded };
enum class LegacyRecipeEra { ImplicitRec2020, ImplicitProPhoto };
inline constexpr std::uint32_t kLegacyRec2020ProcessingVersion = 1;
inline constexpr std::uint32_t kCurrentEditProcessingVersion = 2;

struct IccProfileIdentity {
    std::array<std::uint8_t, 32> profile_sha256{};
    std::string intent = "relative_colorimetric";
    bool black_point_compensation = false;
    std::string engine = "lcms2-core";
    std::string engine_version = "2.19.1";
    bool operator==(const IccProfileIdentity&) const = default;
};

struct EditSource {
    std::string id; // Lowercase UUID; stable across revisions.
    EditSourceKind kind = EditSourceKind::SceneLinearRasterF32;
    // Host-supplied SHA-256 of canonical source metadata plus pixel/sensor data.
    std::array<std::uint8_t, 32> content_sha256{};
    std::optional<IccProfileIdentity> icc_input;
    bool operator==(const EditSource&) const = default;
};

struct EditOperation {
    std::string id; // Lowercase UUID; never reused for a different operation.
    std::string type_id; // Stable identifier, e.g. rawengine.exposure.
    std::uint32_t schema_version = 1;
    std::uint32_t processing_version = 1;
    bool enabled = true;
    EditDomain input_domain = EditDomain::SceneLinearProPhotoD50;
    EditDomain output_domain = EditDomain::SceneLinearProPhotoD50;
    EditValue::Object parameters;
    std::map<std::string, std::string> inputs; // Named edges to source/operation IDs.
    std::map<std::string, std::string> masks;
    std::string blend_mode = "normal";
    double opacity = 1.0;
    EditValue::Object extra_fields; // Unknown operation fields survive replay/save.
    bool operator==(const EditOperation&) const = default;
};

struct EditManifest {
    std::uint32_t format_version = 1;
    std::uint32_t processing_version = kCurrentEditProcessingVersion;
    WorkingSpace working_space = WorkingSpace::LinearProPhotoD50;
    std::vector<EditSource> sources;
    std::vector<EditOperation> operations;
    std::string output_id;
    std::optional<IccProfileIdentity> output_profile;
    EditValue::Object extra_fields;
    bool operator==(const EditManifest&) const = default;
};

// Format v1 rejects missing versions or working space. Unknown operation types
// and extra fields are retained; execution support is a separate question.
RAWENGINE_API void validate_edit_manifest(const EditManifest& manifest);
RAWENGINE_API std::string serialize_edit_manifest(const EditManifest& manifest);
RAWENGINE_API EditManifest parse_edit_manifest(std::string_view json);

// Old GraphRecipe had no file format or processing version. A host importing
// an in-memory legacy recipe must explicitly declare which implicit default
// it came from; no guess is made from current C++ defaults.
RAWENGINE_API EditManifest snapshot_legacy_recipe(
    EditSource source, GraphRecipe recipe, LegacyRecipeEra era,
    std::string operation_id,
    std::optional<IccProfileIdentity> output_profile = std::nullopt);

} // namespace rawengine
