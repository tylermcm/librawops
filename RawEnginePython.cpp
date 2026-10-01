#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "RawEngine.hpp"
#include "EditGraph.hpp"
#include "TileScheduler.hpp"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <iterator>
#include <string_view>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {

struct ModuleState {
    PyObject* job_type = nullptr;
    PyObject* history_type = nullptr;
    PyObject* cancelled_error = nullptr;
};

struct BufferGuard {
    Py_buffer view{};
    ~BufferGuard() { if (view.obj) PyBuffer_Release(&view); }
};

// Native work uses owned source storage; restore the GIL even on exceptions.
struct AllowThreads {
    PyThreadState* state = PyEval_SaveThread();
    ~AllowThreads() { PyEval_RestoreThread(state); }
};

bool read_uint(PyObject* options, const char* key, std::uint32_t& value) {
    PyObject* item = PyDict_GetItemString(options, key);
    if (!item) return true;
    const unsigned long n = PyLong_AsUnsignedLong(item);
    if (PyErr_Occurred()) return false;
    if (n > std::numeric_limits<std::uint32_t>::max()) {
        PyErr_SetString(PyExc_OverflowError, "option exceeds uint32 range");
        return false;
    }
    value = static_cast<std::uint32_t>(n);
    return true;
}

bool read_float(PyObject* options, const char* key, float& value) {
    PyObject* item = PyDict_GetItemString(options, key);
    if (!item) return true;
    value = static_cast<float>(PyFloat_AsDouble(item));
    return !PyErr_Occurred();
}

bool read_levels(PyObject* options, const char* key,
                 std::array<std::uint16_t, 4>& levels) {
    PyObject* item = PyDict_GetItemString(options, key);
    if (!item) return true;
    PyObject* sequence = PySequence_Fast(item, "site levels must be a sequence of four uint16 values");
    if (!sequence) return false;
    if (PySequence_Fast_GET_SIZE(sequence) != 4) {
        Py_DECREF(sequence);
        PyErr_SetString(PyExc_ValueError, "site levels must contain exactly four values");
        return false;
    }
    for (Py_ssize_t site = 0; site < 4; ++site) {
        const auto n = PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(sequence, site));
        if (PyErr_Occurred()) { Py_DECREF(sequence); return false; }
        if (n > 65535) {
            Py_DECREF(sequence);
            PyErr_SetString(PyExc_ValueError, "site levels must be uint16 values");
            return false;
        }
        levels[static_cast<std::size_t>(site)] = static_cast<std::uint16_t>(n);
    }
    Py_DECREF(sequence);
    return true;
}

bool read_color_transform(PyObject* options, rawengine::GraphRecipe& recipe) {
    PyObject* item = PyDict_GetItemString(options, "camera_to_xyz_d50");
    PyObject* target = PyDict_GetItemString(options, "working_space");
    if (!item) {
        if (target) PyErr_SetString(PyExc_ValueError,
                                    "working_space requires camera_to_xyz_d50");
        return !target;
    }
    rawengine::CameraColorTransform transform;
    PyObject* sequence = PySequence_Fast(item, "camera_to_xyz_d50 must be nine row-major numbers");
    if (!sequence) return false;
    if (PySequence_Fast_GET_SIZE(sequence) != 9) {
        Py_DECREF(sequence);
        PyErr_SetString(PyExc_ValueError, "camera_to_xyz_d50 must contain nine numbers");
        return false;
    }
    for (Py_ssize_t i = 0; i < 9; ++i) {
        transform.camera_to_xyz_d50[static_cast<std::size_t>(i)] =
            PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, i));
        if (PyErr_Occurred()) { Py_DECREF(sequence); return false; }
    }
    Py_DECREF(sequence);
    if (target) {
        const char* name = PyUnicode_AsUTF8(target);
        if (!name) return false;
        if (std::string_view(name) == "prophoto-d50")
            transform.target = rawengine::WorkingSpace::LinearProPhotoD50;
        else if (std::string_view(name) == "rec2020-d65")
            transform.target = rawengine::WorkingSpace::LinearRec2020D65;
        else {
            PyErr_SetString(PyExc_ValueError,
                            "working_space must be prophoto-d50 or rec2020-d65");
            return false;
        }
    }
    recipe.camera_color = transform;
    return true;
}

bool read_output_mode(PyObject* options, rawengine::GraphRecipe& recipe) {
    PyObject* item = PyDict_GetItemString(options, "output_mode");
    if (!item) return true;
    const char* name = PyUnicode_AsUTF8(item);
    if (!name) return false;
    if (std::string_view(name) == "legacy")
        recipe.output_mode = rawengine::OutputMode::LegacyBounded;
    else if (std::string_view(name) == "srgb-preview")
        recipe.output_mode = rawengine::OutputMode::SrgbPreview;
    else {
        PyErr_SetString(PyExc_ValueError, "output_mode must be legacy or srgb-preview");
        return false;
    }
    return true;
}

bool read_render_level(PyObject* options, rawengine::RenderLevel& level) {
    if (!read_uint(options, "mip", level.mip)) return false;
    if (level.mip > 2) {
        PyErr_SetString(PyExc_ValueError, "mip must be 0, 1 or 2");
        return false;
    }
    if (PyObject* item = PyDict_GetItemString(options, "quality")) {
        const char* name = PyUnicode_AsUTF8(item);
        if (!name) return false;
        if (std::string_view(name) == "preview") level.quality = rawengine::RenderQuality::Preview;
        else if (std::string_view(name) == "final") level.quality = rawengine::RenderQuality::Final;
        else {
            PyErr_SetString(PyExc_ValueError, "quality must be preview or final");
            return false;
        }
    }
    return true;
}

struct OrientationOptions {
    std::uint32_t quarter_turns = 0;
    bool horizontal = false, vertical = false;
    bool active() const { return quarter_turns || horizontal || vertical; }
};

bool read_raster_request(PyObject* options, std::uint32_t width, std::uint32_t height,
                         rawengine::RenderRequest& request, rawengine::GraphRecipe& recipe,
                         std::optional<rawengine::Rect>& crop,
                         std::optional<rawengine::Rect>& resize, rawengine::ResizeFilter& filter,
                         OrientationOptions& orientation) {
    if (PyObject* item = PyDict_GetItemString(options, "crop"); item && item != Py_None) {
        PyObject* sequence = PySequence_Fast(item, "crop must be four integers: x, y, width, height");
        if (!sequence) return false;
        if (PySequence_Fast_GET_SIZE(sequence) != 4) {
            Py_DECREF(sequence);
            PyErr_SetString(PyExc_ValueError, "crop must contain x, y, width and height");
            return false;
        }
        std::array<std::uint32_t, 4> fields{};
        for (Py_ssize_t i = 0; i < 4; ++i) {
            const auto value = PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(sequence, i));
            if (PyErr_Occurred()) { Py_DECREF(sequence); return false; }
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                Py_DECREF(sequence);
                PyErr_SetString(PyExc_OverflowError, "crop exceeds uint32 range");
                return false;
            }
            fields[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(value);
        }
        Py_DECREF(sequence);
        if (!fields[2] || !fields[3] ||
            static_cast<std::uint64_t>(fields[0]) + fields[2] > width ||
            static_cast<std::uint64_t>(fields[1]) + fields[3] > height) {
            PyErr_SetString(PyExc_ValueError, "crop must be nonempty and inside the original source");
            return false;
        }
        crop = rawengine::Rect{fields[0], fields[1], fields[2], fields[3]};
        width = fields[2]; height = fields[3];
    }
    if (PyObject* item = PyDict_GetItemString(options, "rotate")) {
        if (!PyLong_Check(item) || PyBool_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "rotate must be integer clockwise degrees: 0, 90, 180 or 270");
            return false;
        }
        const auto degrees = PyLong_AsLong(item);
        if (PyErr_Occurred()) return false;
        if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
            PyErr_SetString(PyExc_ValueError, "rotate must be 0, 90, 180 or 270 clockwise degrees");
            return false;
        }
        orientation.quarter_turns = static_cast<std::uint32_t>(degrees / 90);
    }
    for (auto field : {std::pair{"flip_horizontal", &orientation.horizontal},
                       std::pair{"flip_vertical", &orientation.vertical}}) {
        if (PyObject* item = PyDict_GetItemString(options, field.first)) {
            if (!PyBool_Check(item)) {
                PyErr_SetString(PyExc_TypeError, "flip options must be bool");
                return false;
            }
            *field.second = item == Py_True;
        }
    }
    if (orientation.quarter_turns % 2) std::swap(width, height);
    if (PyObject* item = PyDict_GetItemString(options, "resize"); item && item != Py_None) {
        PyObject* sequence = PySequence_Fast(item, "resize must contain integer width and height");
        if (!sequence) return false;
        if (PySequence_Fast_GET_SIZE(sequence) != 2) {
            Py_DECREF(sequence);
            PyErr_SetString(PyExc_ValueError, "resize must contain width and height");
            return false;
        }
        std::array<std::uint32_t, 2> fields{};
        for (Py_ssize_t i = 0; i < 2; ++i) {
            PyObject* field = PySequence_Fast_GET_ITEM(sequence, i);
            if (PyBool_Check(field) || !PyLong_Check(field)) {
                Py_DECREF(sequence);
                PyErr_SetString(PyExc_TypeError, "resize dimensions must be integers");
                return false;
            }
            const auto value = PyLong_AsUnsignedLong(field);
            if (PyErr_Occurred()) { Py_DECREF(sequence); return false; }
            if (!value || value > std::numeric_limits<std::uint32_t>::max()) {
                Py_DECREF(sequence);
                PyErr_SetString(PyExc_ValueError, "resize dimensions must be positive uint32 values");
                return false;
            }
            fields[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(value);
        }
        Py_DECREF(sequence);
        width = fields[0]; height = fields[1];
        resize = rawengine::Rect{0, 0, width, height};
    }
    if (PyObject* item = PyDict_GetItemString(options, "resize_filter")) {
        if (!resize || !PyUnicode_Check(item)) {
            PyErr_SetString(PyExc_ValueError, "resize_filter requires resize and a nearest/bilinear/area string");
            return false;
        }
        const char* name = PyUnicode_AsUTF8(item);
        if (!name) return false;
        if (std::strcmp(name, "nearest") == 0) filter = rawengine::ResizeFilter::Nearest;
        else if (std::strcmp(name, "bilinear") == 0) filter = rawengine::ResizeFilter::Bilinear;
        else if (std::strcmp(name, "area") == 0) filter = rawengine::ResizeFilter::Area;
        else { PyErr_SetString(PyExc_ValueError, "resize_filter must be nearest, bilinear or area"); return false; }
    }
    if (!read_render_level(options, request.level)) return false;
    const auto scale = 1u << request.level.mip;
    request.viewport = {0, 0, width / scale + (width % scale != 0),
                              height / scale + (height % scale != 0)};
    return read_uint(options, "x", request.viewport.x) &&
           read_uint(options, "y", request.viewport.y) &&
           read_uint(options, "roi_width", request.viewport.width) &&
           read_uint(options, "roi_height", request.viewport.height) &&
           read_uint(options, "tile_size", request.tile_size) &&
           read_float(options, "exposure_stops", recipe.exposure_stops) &&
           read_float(options, "tone_shoulder", recipe.tone_shoulder) &&
           read_float(options, "tone_gamma", recipe.tone_gamma) &&
           read_output_mode(options, recipe);
}

PyObject* render(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* source = nullptr;
    PyObject* options = Py_None;
    unsigned int width = 0, height = 0;
    static const char* names[] = {"bayer", "width", "height", "options", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OII|O", const_cast<char**>(names),
                                     &source, &width, &height, &options)) return nullptr;
    if (options == Py_None) {
        options = PyDict_New();
        if (!options) return nullptr;
    }
    else {
        if (!PyDict_Check(options)) {
            PyErr_SetString(PyExc_TypeError, "options must be a dict");
            return nullptr;
        }
        Py_INCREF(options);
    }
    BufferGuard buffer;
    if (PyObject_GetBuffer(source, &buffer.view, PyBUF_CONTIG_RO) < 0) {
        Py_DECREF(options);
        return nullptr;
    }
    try {
        std::uint32_t black = 0, white = 65535, pattern = 0, tile_size = 256;
        std::uint32_t stride = 0, phase_x = 0, phase_y = 0;
        rawengine::Rect active_area;
        rawengine::Rect roi{0, 0, width, height};
        rawengine::GraphRecipe recipe;
        rawengine::RenderLevel level;
        const bool explicit_roi = PyDict_GetItemString(options, "x") ||
                                  PyDict_GetItemString(options, "y") ||
                                  PyDict_GetItemString(options, "roi_width") ||
                                  PyDict_GetItemString(options, "roi_height");
        const bool valid =
            read_uint(options, "black_level", black) &&
            read_uint(options, "white_level", white) &&
            read_uint(options, "pattern", pattern) &&
            read_uint(options, "row_stride_samples", stride) &&
            read_uint(options, "cfa_phase_x", phase_x) &&
            read_uint(options, "cfa_phase_y", phase_y) &&
            read_uint(options, "active_x", active_area.x) &&
            read_uint(options, "active_y", active_area.y) &&
            read_uint(options, "active_width", active_area.width) &&
            read_uint(options, "active_height", active_area.height) &&
            read_uint(options, "tile_size", tile_size) &&
            read_float(options, "red_gain", recipe.red_gain) &&
            read_float(options, "green_gain", recipe.green_gain) &&
            read_float(options, "blue_gain", recipe.blue_gain) &&
            read_float(options, "exposure_stops", recipe.exposure_stops) &&
            read_float(options, "tone_shoulder", recipe.tone_shoulder) &&
            read_float(options, "tone_gamma", recipe.tone_gamma) &&
            read_color_transform(options, recipe) &&
            read_output_mode(options, recipe) && read_render_level(options, level);
        if (!valid) { Py_DECREF(options); return nullptr; }
        if (!(level.mip == 0 && level.quality == rawengine::RenderQuality::Final) &&
            !(level.mip >= 1 && level.mip <= 2 && level.quality == rawengine::RenderQuality::Preview &&
              recipe.camera_color && recipe.output_mode == rawengine::OutputMode::SrgbPreview))
            throw std::invalid_argument("RAW preview requires mip 1/2, preview quality, camera calibration and sRGB output");
        if (PyDict_GetItemString(options, "crop"))
            throw std::invalid_argument("RAW render does not accept raster crop; use active area or ROI");
        if (PyDict_GetItemString(options, "resize") || PyDict_GetItemString(options, "resize_filter"))
            throw std::invalid_argument("RAW render does not accept raster resize");
        if (PyDict_GetItemString(options, "rotate") || PyDict_GetItemString(options, "flip_horizontal") ||
            PyDict_GetItemString(options, "flip_vertical"))
            throw std::invalid_argument("RAW render does not accept raster orientation options");
        if (black > 65535 || white > 65535 || pattern > 3 || phase_x > 1 || phase_y > 1)
            throw std::invalid_argument("invalid levels, Bayer pattern, or CFA phase");
        rawengine::RawMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.row_stride_samples = stride;
        metadata.pattern = static_cast<rawengine::BayerPattern>(pattern);
        metadata.cfa_phase_x = static_cast<std::uint8_t>(phase_x);
        metadata.cfa_phase_y = static_cast<std::uint8_t>(phase_y);
        metadata.active_area = active_area;
        metadata.black_levels.fill(static_cast<std::uint16_t>(black));
        metadata.white_levels.fill(static_cast<std::uint16_t>(white));
        if (!read_levels(options, "black_levels", metadata.black_levels) ||
            !read_levels(options, "white_levels", metadata.white_levels)) {
            Py_DECREF(options);
            return nullptr;
        }
        if (!explicit_roi && active_area.width && active_area.height)
            roi = active_area;
        if (level.mip) {
            const auto scale = 1u << level.mip;
            const auto area_width = active_area.width ? active_area.width : width;
            const auto area_height = active_area.height ? active_area.height : height;
            roi = {0, 0, area_width / scale + (area_width % scale != 0),
                         area_height / scale + (area_height % scale != 0)};
        }
        if (!read_uint(options, "x", roi.x) || !read_uint(options, "y", roi.y) ||
            !read_uint(options, "roi_width", roi.width) ||
            !read_uint(options, "roi_height", roi.height)) {
            Py_DECREF(options);
            return nullptr;
        }
        Py_DECREF(options);
        options = nullptr;
        const auto actual_stride = stride ? stride : width;
        const auto count = static_cast<std::uint64_t>(actual_stride) * height;
        if (!width || !height || count > std::numeric_limits<std::size_t>::max() / 2 ||
            count > static_cast<std::uint64_t>(PY_SSIZE_T_MAX) / 2 ||
            buffer.view.len != static_cast<Py_ssize_t>(count * 2))
            throw std::invalid_argument("bayer must contain row_stride_samples*height native-endian uint16 samples");
        std::vector<std::uint16_t> samples(static_cast<std::size_t>(count));
        std::memcpy(samples.data(), buffer.view.buf, static_cast<std::size_t>(count) * 2);
        std::vector<float> pixels;
        {
            AllowThreads unlocked;
            rawengine::RawImage raw(metadata, std::move(samples));
            rawengine::ImageGraph graph(std::move(raw), recipe);
            pixels = rawengine::Renderer{}.render_image(
                graph, rawengine::RenderRequest{roi, tile_size, level}).rgb;
        }
        if (pixels.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX) / sizeof(float))
            throw std::length_error("requested ROI exceeds Python bytes capacity");
        PyObject* bytes = PyBytes_FromStringAndSize(
            reinterpret_cast<const char*>(pixels.data()),
            static_cast<Py_ssize_t>(pixels.size() * sizeof(float)));
        if (!bytes) return nullptr;
        return Py_BuildValue("IIN", roi.width, roi.height, bytes);
    } catch (const std::exception& error) {
        Py_XDECREF(options);
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, error.what());
        return nullptr;
    }
}

PyObject* render_raster(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* source = nullptr;
    PyObject* options = Py_None;
    unsigned int width = 0, height = 0;
    static const char* names[] = {"rgb", "width", "height", "options", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OII|O", const_cast<char**>(names),
                                     &source, &width, &height, &options)) return nullptr;
    if (options == Py_None) {
        options = PyDict_New();
        if (!options) return nullptr;
    } else {
        if (!PyDict_Check(options)) {
            PyErr_SetString(PyExc_TypeError, "options must be a dict");
            return nullptr;
        }
        Py_INCREF(options);
    }
    BufferGuard buffer;
    if (PyObject_GetBuffer(source, &buffer.view, PyBUF_CONTIG_RO) < 0) {
        Py_DECREF(options);
        return nullptr;
    }
    try {
        std::uint32_t stride = 0;
        rawengine::RenderRequest request;
        rawengine::GraphRecipe recipe;
        std::optional<rawengine::Rect> crop, resize;
        auto resize_filter = rawengine::ResizeFilter::Bilinear;
        OrientationOptions orientation;
        const bool valid =
            read_uint(options, "row_stride_pixels", stride) &&
            read_raster_request(options, width, height, request, recipe, crop, resize, resize_filter, orientation);
        if (!valid) { Py_DECREF(options); return nullptr; }
        if (PyDict_GetItemString(options, "red_gain") ||
            PyDict_GetItemString(options, "green_gain") ||
            PyDict_GetItemString(options, "blue_gain") ||
            PyDict_GetItemString(options, "camera_to_xyz_d50"))
            throw std::invalid_argument("raster input does not accept RAW calibration controls");
        PyObject* target = PyDict_GetItemString(options, "working_space");
        if (!target) {
            Py_DECREF(options);
            PyErr_SetString(PyExc_ValueError, "raster working_space is required");
            return nullptr;
        }
        const char* name = PyUnicode_AsUTF8(target);
        if (!name) { Py_DECREF(options); return nullptr; }
        rawengine::WorkingSpace space;
        if (std::string_view(name) == "prophoto-d50")
            space = rawengine::WorkingSpace::LinearProPhotoD50;
        else if (std::string_view(name) == "rec2020-d65")
            space = rawengine::WorkingSpace::LinearRec2020D65;
        else {
            Py_DECREF(options);
            PyErr_SetString(PyExc_ValueError,
                            "working_space must be prophoto-d50 or rec2020-d65");
            return nullptr;
        }
        Py_DECREF(options);
        options = nullptr;
        const auto actual_stride = stride ? stride : width;
        const auto rows = static_cast<std::uint64_t>(actual_stride) * height;
        if (!width || !height || rows > std::numeric_limits<std::size_t>::max() / 12 ||
            rows > static_cast<std::uint64_t>(PY_SSIZE_T_MAX) / 12 ||
            buffer.view.len != static_cast<Py_ssize_t>(rows * 12))
            throw std::invalid_argument(
                "rgb must contain row_stride_pixels*height native-endian float32 RGB pixels");
        std::vector<float> pixels(static_cast<std::size_t>(rows * 3));
        std::memcpy(pixels.data(), buffer.view.buf, static_cast<std::size_t>(rows * 12));
        rawengine::Tile result;
        {
            AllowThreads unlocked;
            rawengine::RasterImage raster({width, height, stride, space}, std::move(pixels));
            const rawengine::Rect original_bounds{0, 0, width, height};
            std::shared_ptr<const rawengine::Node> node = std::make_shared<rawengine::RasterSourceNode>(std::move(raster));
            auto output_bounds = original_bounds;
            if (crop) {
                node = std::make_shared<rawengine::CropNode>(node, original_bounds, *crop);
                output_bounds = {0, 0, crop->width, crop->height};
            }
            if (orientation.active()) {
                auto oriented = std::make_shared<rawengine::OrientationNode>(node, output_bounds,
                    orientation.quarter_turns, orientation.horizontal, orientation.vertical);
                output_bounds = oriented->output_bounds(); node = std::move(oriented);
            }
            if (resize) {
                node = std::make_shared<rawengine::ResizeNode>(node, output_bounds,
                    resize->width, resize->height, resize_filter);
                output_bounds = *resize;
            }
            rawengine::ImageGraph graph(node, output_bounds, recipe);
            result = rawengine::Renderer{}.render_image(graph, request);
        }
        if (result.rgb.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX) / sizeof(float))
            throw std::length_error("requested ROI exceeds Python bytes capacity");
        PyObject* bytes = PyBytes_FromStringAndSize(
            reinterpret_cast<const char*>(result.rgb.data()),
            static_cast<Py_ssize_t>(result.rgb.size() * sizeof(float)));
        if (!bytes) return nullptr;
        return Py_BuildValue("IIN", result.bounds.width, result.bounds.height, bytes);
    } catch (const std::exception& error) {
        Py_XDECREF(options);
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, error.what());
        return nullptr;
    }
}

struct JobProgress {
    std::atomic<std::uint64_t> completed{0};
    std::uint64_t total = 0;
};

class ProgressNode final : public rawengine::Node {
public:
    ProgressNode(std::shared_ptr<const rawengine::Node> node, std::shared_ptr<JobProgress> progress)
        : node_(std::move(node)), progress_(std::move(progress)) {}
    rawengine::Tile render(rawengine::Rect bounds) const override { return render_level(bounds, {}); }
    rawengine::Tile render_level(rawengine::Rect bounds, rawengine::RenderLevel level) const override {
        auto tile = node_->render_level(bounds, level);
        progress_->completed.fetch_add(1, std::memory_order_relaxed);
        return tile;
    }
    bool supports_level(rawengine::RenderLevel level) const noexcept override {
        return node_->supports_level(level);
    }
    rawengine::ImageDescriptor output_descriptor() const noexcept override {
        return node_->output_descriptor();
    }
private:
    std::shared_ptr<const rawengine::Node> node_;
    std::shared_ptr<JobProgress> progress_;
};

struct RenderJobState {
    std::shared_future<rawengine::Tile> result;
    std::shared_ptr<rawengine::CancellationToken> cancellation = std::make_shared<rawengine::CancellationToken>();
    std::shared_ptr<JobProgress> progress = std::make_shared<JobProgress>();
    std::shared_ptr<rawengine::TileScheduler> scheduler;
    ~RenderJobState() { cancellation->cancel(); }
};

struct JobStateDeleter {
    void operator()(RenderJobState* state) const {
        AllowThreads unlocked;
        delete state;
    }
};

class SessionClosed : public std::logic_error {
public:
    SessionClosed() : std::logic_error("render session is closed") {}
};

struct SessionState {
    rawengine::EditSource source;
    std::shared_ptr<const rawengine::Node> node;
    std::optional<rawengine::RawMetadata> raw_metadata;
    rawengine::Rect bounds{};
    std::vector<rawengine::BoundEditSource> sources;
    mutable std::mutex sources_mutex;
    bool multiple_sources = false;
    std::shared_ptr<rawengine::TileCache> cache;
    std::size_t cache_bytes;
    std::size_t workers, max_pending;
    std::mutex scheduler_mutex;
    std::shared_ptr<rawengine::TileScheduler> scheduler;
    std::vector<std::weak_ptr<rawengine::CancellationToken>> jobs;
    std::atomic<bool> closed{false};

    SessionState(rawengine::RasterImage image, std::size_t budget,
                       std::size_t worker_count, std::size_t pending_budget)
        : bounds{0, 0, image.width(), image.height()},
          cache(std::make_shared<rawengine::TileCache>(budget)), cache_bytes(budget),
          workers(worker_count), max_pending(pending_budget) {
        if (!workers || workers > 64 || !max_pending)
            throw std::invalid_argument("session needs 1-64 workers and a positive pending budget");
        source.id = "00000000-0000-0000-0000-000000000001";
        source.kind = rawengine::EditSourceKind::SceneLinearRasterF32;
        source.working_space = image.metadata().working_space;
        source.content_sha256 = rawengine::fingerprint_raster_source(image);
        node = std::make_shared<rawengine::RasterSourceNode>(std::move(image));
        sources.push_back({source, node, bounds});
    }

    SessionState(rawengine::RawImage image, std::size_t budget,
                 std::size_t worker_count, std::size_t pending_budget,
                 rawengine::RawDemosaicIdentity demosaic)
        : raw_metadata(image.metadata()), bounds(image.metadata().active_area),
          cache(std::make_shared<rawengine::TileCache>(budget)), cache_bytes(budget),
          workers(worker_count), max_pending(pending_budget) {
        if (!workers || workers > 64 || !max_pending)
            throw std::invalid_argument("session needs 1-64 workers and a positive pending budget");
        source.id = "00000000-0000-0000-0000-000000000001";
        source.kind = rawengine::EditSourceKind::DecodedBayerU16;
        source.content_sha256 = image.fingerprint();
        source.demosaic = demosaic;
        node = std::make_shared<rawengine::RawUnpackNode>(std::move(image), std::move(demosaic));
        sources.push_back({source, node, bounds});
    }

    SessionState(std::vector<rawengine::BoundEditSource> bindings, std::size_t budget,
                       std::size_t worker_count, std::size_t pending_budget)
        : sources(std::move(bindings)), multiple_sources(true),
          cache(std::make_shared<rawengine::TileCache>(budget)), cache_bytes(budget),
          workers(worker_count), max_pending(pending_budget) {
        if (!workers || workers > 64 || !max_pending || sources.empty() || sources.size() > 64)
            throw std::invalid_argument("graph session needs 1-64 sources/workers and a positive pending budget");
        rawengine::EditManifest manifest;
        manifest.working_space = sources.front().identity.working_space.value_or(rawengine::WorkingSpace::LinearProPhotoD50);
        for (const auto& binding : sources) manifest.sources.push_back(binding.identity);
        if (std::any_of(manifest.sources.begin(), manifest.sources.end(),
                        [](const auto& s) { return s.demosaic.has_value(); }))
            manifest.format_version = 3;
        manifest.output_id = manifest.sources.front().id;
        rawengine::validate_edit_manifest(manifest); // Includes canonical UUID uniqueness.
    }

    std::vector<rawengine::BoundEditSource> source_snapshot() const {
        std::lock_guard lock(sources_mutex);
        return sources;
    }

    void replace_source(rawengine::BoundEditSource replacement) {
        std::lock_guard lock(sources_mutex);
        if (closed.load()) throw SessionClosed();
        auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& binding) {
            return binding.identity.id == replacement.identity.id;
        });
        if (found == sources.end()) throw std::invalid_argument("source ID is not owned by this graph session");
        *found = std::move(replacement);
        // Source/op signatures invalidate changed branches; old jobs retain old nodes.
    }

    rawengine::ExecutableEditGraph graph(const rawengine::GraphRecipe& recipe,
                                        std::optional<rawengine::Rect> crop = std::nullopt,
                                        std::optional<rawengine::Rect> resize = std::nullopt,
                                        rawengine::ResizeFilter resize_filter = rawengine::ResizeFilter::Bilinear,
                                        OrientationOptions orientation = {}) const {
        using namespace rawengine;
        if (closed.load()) throw SessionClosed();
        EditManifest manifest;
        if (raw_metadata) manifest.format_version = 3;
        manifest.working_space = raw_metadata
            ? (recipe.camera_color ? recipe.camera_color->target : WorkingSpace::LinearProPhotoD50)
            : *source.working_space;
        manifest.sources.push_back(source);
        const auto working_domain = manifest.working_space == WorkingSpace::LinearProPhotoD50
            ? EditDomain::SceneLinearProPhotoD50 : EditDomain::SceneLinearRec2020D65;
        auto domain = raw_metadata ? EditDomain::CameraLinear : working_domain;
        std::string upstream = source.id;
        auto append = [&](const char* id, const char* type, EditDomain output,
                          EditValue::Object parameters = {}) {
            EditOperation op;
            op.id = id;
            op.type_id = type;
            op.processing_version = kCurrentEditProcessingVersion;
            op.input_domain = domain;
            op.output_domain = output;
            op.inputs.emplace("image", upstream);
            op.parameters = std::move(parameters);
            upstream = op.id;
            domain = output;
            manifest.operations.push_back(std::move(op));
        };
        if (raw_metadata) {
            append("00000000-0000-0000-0000-000000000010", "rawengine.white_balance", domain,
                   {{"red_gain", EditValue{static_cast<double>(recipe.red_gain)}},
                    {"green_gain", EditValue{static_cast<double>(recipe.green_gain)}},
                    {"blue_gain", EditValue{static_cast<double>(recipe.blue_gain)}}});
            // Match one-shot RAW stage order, including native exposure rounding.
            append("00000000-0000-0000-0000-000000000002", "rawengine.exposure", domain,
                   {{"stops", EditValue{static_cast<double>(recipe.exposure_stops)}}});
            if (recipe.camera_color) {
                EditValue::Array matrix;
                for (double value : recipe.camera_color->camera_to_xyz_d50) matrix.emplace_back(value);
                append("00000000-0000-0000-0000-000000000011", "rawengine.camera_to_working", working_domain,
                       {{"matrix", EditValue{std::move(matrix)}}});
            }
        }
        if (crop)
            append("00000000-0000-0000-0000-000000000007", "rawengine.crop", domain,
                   {{"x", EditValue{static_cast<std::int64_t>(crop->x)}},
                    {"y", EditValue{static_cast<std::int64_t>(crop->y)}},
                    {"width", EditValue{static_cast<std::int64_t>(crop->width)}},
                    {"height", EditValue{static_cast<std::int64_t>(crop->height)}}});
        if (orientation.active())
            append("00000000-0000-0000-0000-000000000009", "rawengine.orientation", domain,
                   {{"quarter_turns", EditValue{static_cast<std::int64_t>(orientation.quarter_turns)}},
                    {"flip_horizontal", EditValue{orientation.horizontal}}, {"flip_vertical", EditValue{orientation.vertical}}});
        if (resize)
            append("00000000-0000-0000-0000-000000000008", "rawengine.resize", domain,
                   {{"width", EditValue{static_cast<std::int64_t>(resize->width)}},
                    {"height", EditValue{static_cast<std::int64_t>(resize->height)}},
                    {"filter", EditValue{std::string(resize_filter == ResizeFilter::Nearest ? "nearest" :
                        resize_filter == ResizeFilter::Area ? "area" : "bilinear")}}});
        if (!raw_metadata)
            append("00000000-0000-0000-0000-000000000002", "rawengine.exposure", domain,
                   {{"stops", EditValue{static_cast<double>(recipe.exposure_stops)}}});
        if (recipe.output_mode == OutputMode::SrgbPreview)
            append("00000000-0000-0000-0000-000000000003", "rawengine.working_to_srgb",
                   EditDomain::SceneLinearSrgb);
        append("00000000-0000-0000-0000-000000000004", "rawengine.tone_curve",
               recipe.output_mode == OutputMode::SrgbPreview
                   ? EditDomain::DisplayLinearSrgb : EditDomain::ToneMappedUnmanaged,
               {{"shoulder", EditValue{static_cast<double>(recipe.tone_shoulder)}},
                {"gamma", EditValue{static_cast<double>(recipe.tone_gamma)}}});
        if (recipe.output_mode == OutputMode::SrgbPreview)
            append("00000000-0000-0000-0000-000000000005", "rawengine.srgb_encode",
                   EditDomain::DisplayEncodedSrgb);
        else
            append("00000000-0000-0000-0000-000000000006", "rawengine.output_clip",
                   EditDomain::UnmanagedBounded);
        manifest.output_id = upstream;
        return ExecutableEditGraph(std::move(manifest), {{source, node, bounds}}, nullptr, cache);
    }

    rawengine::ExecutableEditGraph manifest_graph(std::string_view json) const {
        if (closed.load()) throw SessionClosed();
        auto manifest = rawengine::parse_edit_manifest(json);
        // Binding validates the complete saved identity, not just its UUID.
        auto bindings = source_snapshot();
        if (multiple_sources) {
            std::vector<rawengine::BoundEditSource> selected;
            for (const auto& record : manifest.sources) {
                auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
                    return binding.identity.id == record.id;
                });
                if (found == bindings.end()) throw std::invalid_argument("manifest source ID is not owned by this graph session");
                selected.push_back(*found);
            }
            bindings = std::move(selected);
        }
        return rawengine::ExecutableEditGraph(std::move(manifest), std::move(bindings), nullptr, cache);
    }

    void submit(RenderJobState& job, const rawengine::ExecutableEditGraph& view,
                rawengine::RenderRequest request, rawengine::RenderPriority priority,
                const std::string& group) {
        if (!request.tile_size) throw std::invalid_argument("tile size must be positive");
        auto output = view.output_handle();
        const auto output_bounds = view.output_bounds();
        const auto columns = request.viewport.width / request.tile_size +
                             (request.viewport.width % request.tile_size != 0);
        const auto rows = request.viewport.height / request.tile_size +
                          (request.viewport.height % request.tile_size != 0);
        job.progress->total = static_cast<std::uint64_t>(columns) * rows;
        output = std::make_shared<ProgressNode>(std::move(output), job.progress);
        std::lock_guard lock(scheduler_mutex);
        if (closed.load()) throw SessionClosed();
        if (!scheduler) scheduler = std::make_shared<rawengine::TileScheduler>(workers, max_pending);
        // Allocate bookkeeping before dispatch so failed submission cannot lose a token.
        std::erase_if(jobs, [](const auto& weak) {
            auto token = weak.lock();
            return !token || token->is_cancelled();
        });
        jobs.push_back(job.cancellation);
        job.scheduler = scheduler;
        job.result = (group.empty()
            ? scheduler->submit(output, output_bounds, request, priority, job.cancellation)
            : scheduler->submit_latest(group, output, output_bounds, request, priority, job.cancellation)).share();
    }

    void close() {
        std::shared_ptr<rawengine::TileScheduler> owned_scheduler;
        {
            std::lock_guard lock(scheduler_mutex);
            closed.store(true);
            for (const auto& weak : jobs)
                if (auto token = weak.lock()) token->cancel();
            jobs.clear();
            owned_scheduler = std::move(scheduler);
        }
        // Render jobs can retain the scheduler; its final destruction joins workers.
    }
};

struct SessionObject {
    PyObject_HEAD
    SessionState* state;
};

// Manifest options describe a render request only. Edits and output policy
// belong to the saved graph, so recipe keys must never be silently ignored.
bool read_manifest_request(PyObject* options, rawengine::Rect bounds, rawengine::RenderRequest& request) {
    Py_ssize_t position = 0;
    PyObject *key, *value;
    while (PyDict_Next(options, &position, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "manifest request keys must be strings");
            return false;
        }
        Py_ssize_t length = 0;
        const char* text = PyUnicode_AsUTF8AndSize(key, &length);
        if (!text) return false;
        const std::string_view name(text, static_cast<std::size_t>(length));
        if (name != "x" && name != "y" && name != "roi_width" && name != "roi_height" &&
            name != "tile_size" && name != "mip" && name != "quality") {
            PyErr_SetString(PyExc_ValueError, "manifest options accept only ROI, tile_size, mip and quality; edits belong in the manifest");
            return false;
        }
    }
    if (!read_render_level(options, request.level)) return false;
    const auto scale = 1u << request.level.mip;
    request.viewport = {request.level.mip ? 0 : bounds.x, request.level.mip ? 0 : bounds.y,
        bounds.width / scale + (bounds.width % scale != 0),
        bounds.height / scale + (bounds.height % scale != 0)};
    return read_uint(options, "x", request.viewport.x) && read_uint(options, "y", request.viewport.y) &&
           read_uint(options, "roi_width", request.viewport.width) &&
           read_uint(options, "roi_height", request.viewport.height) &&
           read_uint(options, "tile_size", request.tile_size);
}

bool prepare_session_graph(SessionState& state, PyObject* options, PyObject* manifest,
                           rawengine::RenderRequest& request,
                           std::optional<rawengine::ExecutableEditGraph>& graph) {
    if (manifest) {
        Py_ssize_t length = 0;
        const char* text = PyUnicode_AsUTF8AndSize(manifest, &length);
        if (!text) return false;
        const std::string json(text, static_cast<std::size_t>(length));
        {
            AllowThreads unlocked;
            graph.emplace(state.manifest_graph(json));
        }
        return read_manifest_request(options, graph->output_bounds(), request);
    }
    if (state.raw_metadata) {
        Py_ssize_t position = 0;
        PyObject *key, *value;
        while (PyDict_Next(options, &position, &key, &value)) {
            Py_ssize_t length = 0;
            const char* text = PyUnicode_AsUTF8AndSize(key, &length);
            if (!text) return false;
            const std::string_view name(text, static_cast<std::size_t>(length));
            const std::string_view allowed[] = {"x", "y", "roi_width", "roi_height", "tile_size", "mip", "quality",
                "red_gain", "green_gain", "blue_gain", "camera_to_xyz_d50", "working_space",
                "exposure_stops", "tone_shoulder", "tone_gamma", "output_mode"};
            if (std::find(std::begin(allowed), std::end(allowed), name) == std::end(allowed))
                throw std::invalid_argument("unknown RAW recipe option; sensor metadata and session budgets are fixed");
        }
        rawengine::GraphRecipe recipe;
        if (!read_float(options, "red_gain", recipe.red_gain) ||
            !read_float(options, "green_gain", recipe.green_gain) ||
            !read_float(options, "blue_gain", recipe.blue_gain) ||
            !read_float(options, "exposure_stops", recipe.exposure_stops) ||
            !read_float(options, "tone_shoulder", recipe.tone_shoulder) ||
            !read_float(options, "tone_gamma", recipe.tone_gamma) ||
            !read_color_transform(options, recipe) || !read_output_mode(options, recipe) ||
            !read_render_level(options, request.level)) return false;
        // Recipe and manifest requests share the same native/reduced extent rule.
        const auto scale = 1u << request.level.mip;
        request.viewport = {request.level.mip ? 0 : state.bounds.x, request.level.mip ? 0 : state.bounds.y,
            state.bounds.width / scale + (state.bounds.width % scale != 0),
            state.bounds.height / scale + (state.bounds.height % scale != 0)};
        if (!read_uint(options, "x", request.viewport.x) || !read_uint(options, "y", request.viewport.y) ||
            !read_uint(options, "roi_width", request.viewport.width) ||
            !read_uint(options, "roi_height", request.viewport.height) ||
            !read_uint(options, "tile_size", request.tile_size)) return false;
        {
            AllowThreads unlocked;
            graph.emplace(state.graph(recipe));
        }
        return true;
    }
    for (const char* key : {"working_space", "row_stride_pixels", "red_gain", "green_gain", "blue_gain",
                            "camera_to_xyz_d50", "cache_bytes", "workers", "max_pending"})
        if (PyDict_GetItemString(options, key))
            throw std::invalid_argument("session render options cannot change source, RAW calibration or budgets");
    rawengine::GraphRecipe recipe;
    std::optional<rawengine::Rect> crop, resize;
    auto filter = rawengine::ResizeFilter::Bilinear;
    OrientationOptions orientation;
    if (!read_raster_request(options, state.bounds.width, state.bounds.height, request, recipe,
                             crop, resize, filter, orientation)) return false;
    {
        AllowThreads unlocked;
        graph.emplace(state.graph(recipe, crop, resize, filter, orientation));
    }
    return true;
}

struct CopiedRasterSource {
    std::string id;
    rawengine::RasterMetadata metadata;
    std::vector<float> pixels;
};

bool copy_raster_spec(PyObject* spec, CopiedRasterSource& input) {
    if (!PyDict_Check(spec)) {
        PyErr_SetString(PyExc_TypeError, "each source must be a dict with rgb, width, height and working_space");
        return false;
    }
    Py_ssize_t position = 0;
    PyObject *key, *value;
    while (PyDict_Next(spec, &position, &key, &value)) {
        Py_ssize_t length = 0;
        const char* text = PyUnicode_AsUTF8AndSize(key, &length);
        if (!text) return false;
        const std::string_view name(text, static_cast<std::size_t>(length));
        if (name != "rgb" && name != "width" && name != "height" && name != "working_space" && name != "row_stride_pixels") {
            PyErr_SetString(PyExc_ValueError, "unknown raster source field");
            return false;
        }
    }
    for (auto field : {std::pair{"width", &input.metadata.width},
                       std::pair{"height", &input.metadata.height},
                       std::pair{"row_stride_pixels", &input.metadata.row_stride_pixels}}) {
        PyObject* item = PyDict_GetItemString(spec, field.first);
        if (!item && std::string_view(field.first) == "row_stride_pixels") continue;
        if (!item || !PyLong_Check(item) || PyBool_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "source dimensions/stride must be integers");
            return false;
        }
        if (!read_uint(spec, field.first, *field.second)) return false;
    }
    PyObject* space = PyDict_GetItemString(spec, "working_space");
    if (!space) { PyErr_SetString(PyExc_ValueError, "each source requires working_space"); return false; }
    Py_ssize_t length = 0;
    const char* text = PyUnicode_AsUTF8AndSize(space, &length);
    if (!text) return false;
    const std::string_view name(text, static_cast<std::size_t>(length));
    if (name == "prophoto-d50") input.metadata.working_space = rawengine::WorkingSpace::LinearProPhotoD50;
    else if (name == "rec2020-d65") input.metadata.working_space = rawengine::WorkingSpace::LinearRec2020D65;
    else { PyErr_SetString(PyExc_ValueError, "working_space must be prophoto-d50 or rec2020-d65"); return false; }
    PyObject* rgb = PyDict_GetItemString(spec, "rgb");
    if (!rgb) { PyErr_SetString(PyExc_ValueError, "each source requires rgb"); return false; }
    BufferGuard buffer;
    if (PyObject_GetBuffer(rgb, &buffer.view, PyBUF_CONTIG_RO) < 0) return false;
    const auto& metadata = input.metadata;
    const auto count = static_cast<std::uint64_t>(metadata.row_stride_pixels ? metadata.row_stride_pixels : metadata.width) * metadata.height;
    if (!metadata.width || !metadata.height || (metadata.row_stride_pixels && metadata.row_stride_pixels < metadata.width) ||
        count > std::numeric_limits<std::size_t>::max() / 12 || count > static_cast<std::uint64_t>(PY_SSIZE_T_MAX) / 12 ||
        buffer.view.len != static_cast<Py_ssize_t>(count * 12))
        throw std::invalid_argument("rgb must contain row_stride_pixels*height native-endian float32 RGB pixels with valid dimensions/stride");
    input.pixels.resize(static_cast<std::size_t>(count * 3));
    std::memcpy(input.pixels.data(), buffer.view.buf, static_cast<std::size_t>(count * 12));
    return true;
}

rawengine::BoundEditSource bind_copied_source(CopiedRasterSource input) {
    rawengine::RasterImage image(input.metadata, std::move(input.pixels));
    rawengine::EditSource record;
    record.id = std::move(input.id);
    record.kind = rawengine::EditSourceKind::SceneLinearRasterF32;
    record.working_space = image.metadata().working_space;
    record.content_sha256 = rawengine::fingerprint_raster_source(image);
    const rawengine::Rect bounds{0, 0, image.width(), image.height()};
    return {std::move(record), std::make_shared<rawengine::RasterSourceNode>(std::move(image)), bounds};
}

PyObject* raw_session_new(PyTypeObject* type, PyObject* args, PyObject* kwargs) {
    PyObject *source = nullptr, *metadata_options = Py_None;
    PyObject *budget_object = nullptr, *workers_object = nullptr, *pending_object = nullptr;
    PyObject* demosaic_object = Py_None;
    unsigned int width = 0, height = 0;
    static const char* names[] = {"bayer", "width", "height", "metadata", "cache_bytes", "workers", "max_pending", "demosaic", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OII|O$OOOO", const_cast<char**>(names),
            &source, &width, &height, &metadata_options, &budget_object, &workers_object, &pending_object, &demosaic_object)) return nullptr;
    if (metadata_options != Py_None && !PyDict_Check(metadata_options)) {
        PyErr_SetString(PyExc_TypeError, "metadata must be a dict or None"); return nullptr;
    }
    try {
        rawengine::RawDemosaicIdentity demosaic;
        if (demosaic_object != Py_None) {
            if (!PyDict_Check(demosaic_object)) {
                PyErr_SetString(PyExc_TypeError, "demosaic must be a dict or None"); return nullptr;
            }
            PyObject* algorithm = PyDict_GetItemString(demosaic_object, "algorithm");
            PyObject* version = PyDict_GetItemString(demosaic_object, "processing_version");
            if (PyDict_Size(demosaic_object) != 2 || !algorithm || !version)
                throw std::invalid_argument("demosaic requires exactly algorithm and processing_version");
            if (!PyUnicode_Check(algorithm) || !PyLong_Check(version) || PyBool_Check(version)) {
                PyErr_SetString(PyExc_TypeError, "demosaic requires a string algorithm and integer processing_version"); return nullptr;
            }
            Py_ssize_t length = 0;
            const char* text = PyUnicode_AsUTF8AndSize(algorithm, &length);
            if (!text) return nullptr;
            demosaic.algorithm.assign(text, static_cast<std::size_t>(length));
            const auto number = PyLong_AsUnsignedLong(version);
            if (PyErr_Occurred()) return nullptr;
            if (number > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("invalid demosaic processing_version");
            demosaic.processing_version = static_cast<std::uint32_t>(number);
        }
        rawengine::validate_raw_demosaic(demosaic);
        const auto budget = budget_object ? PyLong_AsSize_t(budget_object) : 64 * 1024 * 1024;
        const auto workers = workers_object ? PyLong_AsSize_t(workers_object) : 1;
        const auto pending = pending_object ? PyLong_AsSize_t(pending_object) : 8;
        if (PyErr_Occurred()) return nullptr;
        rawengine::RawMetadata metadata;
        metadata.width = width; metadata.height = height;
        if (metadata_options != Py_None) {
            Py_ssize_t position = 0;
            PyObject *key, *value;
            while (PyDict_Next(metadata_options, &position, &key, &value)) {
                Py_ssize_t length = 0;
                const char* text = PyUnicode_AsUTF8AndSize(key, &length);
                if (!text) return nullptr;
                const std::string_view name(text, static_cast<std::size_t>(length));
                const std::string_view allowed[] = {"row_stride_samples", "pattern", "cfa_phase_x", "cfa_phase_y",
                    "active_x", "active_y", "active_width", "active_height", "black_level", "white_level", "black_levels", "white_levels"};
                if (std::find(std::begin(allowed), std::end(allowed), name) == std::end(allowed))
                    throw std::invalid_argument("unknown RAW metadata field; editing controls belong in render options");
                if (name != "black_levels" && name != "white_levels" && (!PyLong_Check(value) || PyBool_Check(value))) {
                    PyErr_SetString(PyExc_TypeError, "RAW metadata dimensions, phase, pattern and levels must be integers"); return nullptr;
                }
            }
            std::uint32_t black = 0, white = 65535, pattern = 0, px = 0, py = 0;
            if (!read_uint(metadata_options, "row_stride_samples", metadata.row_stride_samples) ||
                !read_uint(metadata_options, "pattern", pattern) ||
                !read_uint(metadata_options, "cfa_phase_x", px) || !read_uint(metadata_options, "cfa_phase_y", py) ||
                !read_uint(metadata_options, "active_x", metadata.active_area.x) ||
                !read_uint(metadata_options, "active_y", metadata.active_area.y) ||
                !read_uint(metadata_options, "active_width", metadata.active_area.width) ||
                !read_uint(metadata_options, "active_height", metadata.active_area.height) ||
                !read_uint(metadata_options, "black_level", black) || !read_uint(metadata_options, "white_level", white)) return nullptr;
            if (black > 65535 || white > 65535 || pattern > 3 || px > 1 || py > 1)
                throw std::invalid_argument("invalid RAW levels, pattern or CFA phase");
            metadata.pattern = static_cast<rawengine::BayerPattern>(pattern);
            metadata.cfa_phase_x = static_cast<std::uint8_t>(px); metadata.cfa_phase_y = static_cast<std::uint8_t>(py);
            metadata.black_levels.fill(static_cast<std::uint16_t>(black));
            metadata.white_levels.fill(static_cast<std::uint16_t>(white));
            if (!read_levels(metadata_options, "black_levels", metadata.black_levels) ||
                !read_levels(metadata_options, "white_levels", metadata.white_levels)) return nullptr;
        }
        BufferGuard buffer;
        if (PyObject_GetBuffer(source, &buffer.view, PyBUF_CONTIG_RO) < 0) return nullptr;
        const auto stride = metadata.row_stride_samples ? metadata.row_stride_samples : width;
        const auto count = static_cast<std::uint64_t>(stride) * height;
        if (!width || !height || stride < width || count > std::numeric_limits<std::size_t>::max() / 2 ||
            count > static_cast<std::uint64_t>(PY_SSIZE_T_MAX) / 2 ||
            buffer.view.len != static_cast<Py_ssize_t>(count * 2))
            throw std::invalid_argument("bayer must contain row_stride_samples*height native-endian uint16 samples with valid dimensions/stride");
        std::vector<std::uint16_t> samples(static_cast<std::size_t>(count));
        std::memcpy(samples.data(), buffer.view.buf, static_cast<std::size_t>(count * 2));
        std::unique_ptr<SessionState> state;
        {
            AllowThreads unlocked;
            state = std::make_unique<SessionState>(rawengine::RawImage(metadata, std::move(samples)), budget, workers, pending, std::move(demosaic));
        }
        auto* self = reinterpret_cast<SessionObject*>(type->tp_alloc(type, 0));
        if (!self) return nullptr;
        self->state = state.release();
        return reinterpret_cast<PyObject*>(self);
    } catch (const std::bad_alloc&) { return PyErr_NoMemory(); }
    catch (const std::exception& error) { if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, error.what()); return nullptr; }
}

PyObject* graph_session_new(PyTypeObject* type, PyObject* args, PyObject* kwargs) {
    PyObject *specs = nullptr, *budget_object = nullptr, *workers_object = nullptr, *pending_object = nullptr;
    static const char* names[] = {"sources", "cache_bytes", "workers", "max_pending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$OOO", const_cast<char**>(names),
            &specs, &budget_object, &workers_object, &pending_object)) return nullptr;
    if (!PyDict_Check(specs)) { PyErr_SetString(PyExc_TypeError, "sources must map stable UUID strings to raster specs"); return nullptr; }
    try {
        const auto count = PyDict_Size(specs);
        if (count < 1 || count > 64) throw std::invalid_argument("graph session needs 1-64 sources");
        const auto budget = budget_object ? PyLong_AsSize_t(budget_object) : 64 * 1024 * 1024;
        const auto workers = workers_object ? PyLong_AsSize_t(workers_object) : 1;
        const auto pending = pending_object ? PyLong_AsSize_t(pending_object) : 8;
        if (PyErr_Occurred()) return nullptr;
        std::vector<CopiedRasterSource> inputs;
        inputs.reserve(static_cast<std::size_t>(count));
        Py_ssize_t position = 0;
        PyObject *id, *spec;
        // Copy all Python buffers before releasing the GIL or retaining any source.
        while (PyDict_Next(specs, &position, &id, &spec)) {
            Py_ssize_t length = 0;
            const char* text = PyUnicode_AsUTF8AndSize(id, &length);
            if (!text) return nullptr;
            CopiedRasterSource input;
            input.id.assign(text, static_cast<std::size_t>(length));
            if (!copy_raster_spec(spec, input)) return nullptr;
            inputs.push_back(std::move(input));
        }
        std::unique_ptr<SessionState> state;
        {
            AllowThreads unlocked;
            std::vector<rawengine::BoundEditSource> bindings;
            for (auto& input : inputs) bindings.push_back(bind_copied_source(std::move(input)));
            state = std::make_unique<SessionState>(std::move(bindings), budget, workers, pending);
        }
        auto* self = reinterpret_cast<SessionObject*>(type->tp_alloc(type, 0));
        if (!self) return nullptr;
        self->state = state.release();
        return reinterpret_cast<PyObject*>(self);
    } catch (const std::bad_alloc&) { return PyErr_NoMemory(); }
    catch (const std::exception& error) { if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, error.what()); return nullptr; }
}

PyObject* session_new(PyTypeObject* type, PyObject* args, PyObject* kwargs) {
    PyObject* source = nullptr;
    PyObject* budget_object = nullptr;
    PyObject* workers_object = nullptr;
    PyObject* pending_object = nullptr;
    const char* space_name = nullptr;
    unsigned int width = 0, height = 0, stride = 0;
    std::size_t workers = 1, max_pending = 8;
    static const char* names[] = {"rgb", "width", "height", "working_space",
                                  "row_stride_pixels", "cache_bytes", "workers", "max_pending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OIIs|IOOO", const_cast<char**>(names),
            &source, &width, &height, &space_name, &stride, &budget_object, &workers_object, &pending_object)) return nullptr;
    if (workers_object) workers = PyLong_AsSize_t(workers_object);
    if (PyErr_Occurred()) return nullptr;
    if (pending_object) max_pending = PyLong_AsSize_t(pending_object);
    if (PyErr_Occurred()) return nullptr;
    std::size_t budget = 64 * 1024 * 1024;
    if (budget_object) {
        budget = PyLong_AsSize_t(budget_object);
        if (PyErr_Occurred()) return nullptr;
    }
    rawengine::WorkingSpace space;
    if (std::string_view(space_name) == "prophoto-d50")
        space = rawengine::WorkingSpace::LinearProPhotoD50;
    else if (std::string_view(space_name) == "rec2020-d65")
        space = rawengine::WorkingSpace::LinearRec2020D65;
    else {
        PyErr_SetString(PyExc_ValueError, "working_space must be prophoto-d50 or rec2020-d65");
        return nullptr;
    }
    BufferGuard buffer;
    if (PyObject_GetBuffer(source, &buffer.view, PyBUF_CONTIG_RO) < 0) return nullptr;
    try {
        const auto count = static_cast<std::uint64_t>(stride ? stride : width) * height;
        if (!width || !height || count > std::numeric_limits<std::size_t>::max() / 12 ||
            count > static_cast<std::uint64_t>(PY_SSIZE_T_MAX) / 12 ||
            buffer.view.len != static_cast<Py_ssize_t>(count * 12))
            throw std::invalid_argument("rgb must contain row_stride_pixels*height native-endian float32 RGB pixels");
        std::vector<float> pixels(static_cast<std::size_t>(count * 3));
        std::memcpy(pixels.data(), buffer.view.buf, static_cast<std::size_t>(count * 12));
        std::unique_ptr<SessionState> state;
        {
            AllowThreads unlocked;
            state = std::make_unique<SessionState>(
                rawengine::RasterImage({width, height, stride, space}, std::move(pixels)), budget, workers, max_pending);
        }
        auto* self = reinterpret_cast<SessionObject*>(type->tp_alloc(type, 0));
        if (!self) return nullptr;
        self->state = state.release();
        return reinterpret_cast<PyObject*>(self);
    } catch (const std::bad_alloc&) {
        return PyErr_NoMemory();
    } catch (const std::exception& error) {
        PyErr_SetString(PyExc_ValueError, error.what());
        return nullptr;
    }
}

void session_dealloc(PyObject* object) {
    {
        AllowThreads unlocked;
        delete reinterpret_cast<SessionObject*>(object)->state;
    }
    PyTypeObject* type = Py_TYPE(object);
    type->tp_free(object);
    Py_DECREF(type);
}

PyObject* render_session_graph(PyObject* object, PyObject* args, PyObject* kwargs,
                               bool from_manifest, bool export_only = false, bool regions_only = false) {
    PyObject* options = Py_None;
    PyObject* manifest = nullptr;
    static const char* recipe_names[] = {"options", nullptr};
    static const char* manifest_names[] = {"manifest", "options", nullptr};
    if (from_manifest) {
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", const_cast<char**>(manifest_names), &manifest, &options))
            return nullptr;
    } else if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", const_cast<char**>(recipe_names), &options)) return nullptr;
    if (options == Py_None) options = PyDict_New();
    else if (PyDict_Check(options)) Py_INCREF(options);
    else {
        PyErr_SetString(PyExc_TypeError, "options must be a dict");
        return nullptr;
    }
    if (!options) return nullptr;
    auto* state = reinterpret_cast<SessionObject*>(object)->state;
    try {
        rawengine::RenderRequest request;
        std::optional<rawengine::ExecutableEditGraph> graph;
        if (!prepare_session_graph(*state, options, manifest, request, graph)) {
            Py_DECREF(options);
            return nullptr;
        }
        Py_DECREF(options);
        options = nullptr;
        if (regions_only) {
            if (!request.tile_size) throw std::invalid_argument("tile size must be positive");
            std::map<std::string, rawengine::Rect> regions;
            {
                AllowThreads unlocked;
                regions = graph->required_source_regions(request.viewport, request.level);
            }
            PyObject* result = PyDict_New();
            if (!result) return nullptr;
            for (const auto& [id, rect] : regions) {
                PyObject* region = Py_BuildValue("IIII", rect.x, rect.y, rect.width, rect.height);
                if (!region) { Py_DECREF(result); return nullptr; }
                const auto status = PyDict_SetItemString(result, id.c_str(), region);
                Py_DECREF(region);
                if (status < 0) { Py_DECREF(result); return nullptr; }
            }
            return result;
        }
        if (export_only) {
            std::string json;
            {
                AllowThreads unlocked;
                json = rawengine::serialize_edit_manifest(graph->manifest());
            }
            return PyUnicode_FromStringAndSize(json.data(), static_cast<Py_ssize_t>(json.size()));
        }
        rawengine::Tile result;
        {
            AllowThreads unlocked;
            result = rawengine::Renderer{}.render_image(*graph, request);
        }
        if (result.rgb.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX) / sizeof(float))
            throw std::length_error("requested ROI exceeds Python bytes capacity");
        PyObject* bytes = PyBytes_FromStringAndSize(reinterpret_cast<const char*>(result.rgb.data()),
                                                  static_cast<Py_ssize_t>(result.rgb.size() * sizeof(float)));
        if (!bytes) return nullptr;
        return Py_BuildValue("IIN", result.bounds.width, result.bounds.height, bytes);
    } catch (const SessionClosed& error) {
        Py_XDECREF(options);
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return nullptr;
    } catch (const std::bad_alloc&) {
        Py_XDECREF(options);
        return PyErr_NoMemory();
    } catch (const std::exception& error) {
        Py_XDECREF(options);
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, error.what());
        return nullptr;
    }
}

PyObject* session_render(PyObject* object, PyObject* args, PyObject* kwargs) {
    return render_session_graph(object, args, kwargs, false);
}

PyObject* session_render_manifest(PyObject* object, PyObject* args, PyObject* kwargs) {
    return render_session_graph(object, args, kwargs, true);
}

PyObject* session_export_manifest(PyObject* object, PyObject* args, PyObject* kwargs) {
    return render_session_graph(object, args, kwargs, false, true);
}

PyObject* session_required_regions(PyObject* object, PyObject* args, PyObject* kwargs) {
    return render_session_graph(object, args, kwargs, true, false, true);
}

PyObject* source_info_object(const rawengine::EditSource& source, rawengine::Rect bounds) {
    char digest[65]{};
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < source.content_sha256.size(); ++i) {
        digest[2 * i] = hex[source.content_sha256[i] >> 4];
        digest[2 * i + 1] = hex[source.content_sha256[i] & 15];
    }
    return Py_BuildValue("{ss,ss,ss,ss,sI,sI}", "id", source.id.c_str(),
        "kind", "scene_linear_raster_f32", "working_space",
        *source.working_space == rawengine::WorkingSpace::LinearProPhotoD50 ? "linear_prophoto_d50" : "linear_rec2020_d65",
        "content_sha256", digest, "width", bounds.width, "height", bounds.height);
}

PyObject* session_source_info(PyObject* object, PyObject*) {
    const auto* state = reinterpret_cast<SessionObject*>(object)->state;
    if (state->raw_metadata) {
        const auto& source = state->source;
        const auto& m = *state->raw_metadata;
        char digest[65]{};
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t i = 0; i < source.content_sha256.size(); ++i) {
            digest[2 * i] = hex[source.content_sha256[i] >> 4];
            digest[2 * i + 1] = hex[source.content_sha256[i] & 15];
        }
        return Py_BuildValue("{ss,ss,ss,s{ss,sI},sI,sI,sI,sI,sI,sI,s(IIII),s(IIII),s(IIII)}",
            "id", source.id.c_str(), "kind", "decoded_bayer_u16", "content_sha256", digest,
            "demosaic", "algorithm", source.demosaic->algorithm.c_str(),
            "processing_version", source.demosaic->processing_version,
            "width", m.width, "height", m.height, "row_stride_samples", m.row_stride_samples,
            "pattern", static_cast<unsigned>(m.pattern), "cfa_phase_x", static_cast<unsigned>(m.cfa_phase_x),
            "cfa_phase_y", static_cast<unsigned>(m.cfa_phase_y),
            "active_area", m.active_area.x, m.active_area.y, m.active_area.width, m.active_area.height,
            "black_levels", static_cast<unsigned>(m.black_levels[0]), static_cast<unsigned>(m.black_levels[1]),
                            static_cast<unsigned>(m.black_levels[2]), static_cast<unsigned>(m.black_levels[3]),
            "white_levels", static_cast<unsigned>(m.white_levels[0]), static_cast<unsigned>(m.white_levels[1]),
                            static_cast<unsigned>(m.white_levels[2]), static_cast<unsigned>(m.white_levels[3]));
    }
    return source_info_object(state->source, state->bounds);
}

PyObject* graph_session_source_info(PyObject* object, PyObject*) {
    try {
        const auto bindings = reinterpret_cast<SessionObject*>(object)->state->source_snapshot();
        PyObject* result = PyDict_New();
        if (!result) return nullptr;
        for (const auto& binding : bindings) {
            PyObject* info = source_info_object(binding.identity, binding.bounds);
            if (!info) { Py_DECREF(result); return nullptr; }
            const auto status = PyDict_SetItemString(result, binding.identity.id.c_str(), info);
            Py_DECREF(info);
            if (status < 0) { Py_DECREF(result); return nullptr; }
        }
        return result;
    } catch (const std::bad_alloc&) { return PyErr_NoMemory(); }
    catch (const std::exception& error) { PyErr_SetString(PyExc_RuntimeError, error.what()); return nullptr; }
}

PyObject* graph_session_export_manifest(PyObject* object, PyObject* args, PyObject* kwargs) {
    const char* id = nullptr;
    static const char* names[] = {"source_id", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s", const_cast<char**>(names), &id)) return nullptr;
    const auto* state = reinterpret_cast<SessionObject*>(object)->state;
    try {
        std::string json;
        {
            AllowThreads unlocked;
            if (state->closed.load()) throw SessionClosed();
            const auto bindings = state->source_snapshot();
            rawengine::EditManifest manifest;
            const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
                return binding.identity.id == id;
            });
            if (found == bindings.end()) throw std::invalid_argument("source ID is not owned by this graph session");
            manifest.working_space = *found->identity.working_space;
            for (const auto& binding : bindings) manifest.sources.push_back(binding.identity);
            manifest.output_id = id;
            json = rawengine::serialize_edit_manifest(manifest);
        }
        return PyUnicode_FromStringAndSize(json.data(), static_cast<Py_ssize_t>(json.size()));
    } catch (const SessionClosed& error) { PyErr_SetString(PyExc_RuntimeError, error.what()); return nullptr; }
    catch (const std::bad_alloc&) { return PyErr_NoMemory(); }
    catch (const std::exception& error) { PyErr_SetString(PyExc_ValueError, error.what()); return nullptr; }
}

PyObject* graph_session_replace_source(PyObject* object, PyObject* args, PyObject* kwargs) {
    const char* id = nullptr;
    PyObject* spec = nullptr;
    static const char* names[] = {"source_id", "source", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO", const_cast<char**>(names), &id, &spec)) return nullptr;
    auto* state = reinterpret_cast<SessionObject*>(object)->state;
    try {
        if (state->closed.load()) throw SessionClosed();
        CopiedRasterSource input;
        input.id = id;
        if (!copy_raster_spec(spec, input)) return nullptr;
        {
            AllowThreads unlocked;
            state->replace_source(bind_copied_source(std::move(input)));
        }
        Py_RETURN_NONE;
    } catch (const SessionClosed& error) { PyErr_SetString(PyExc_RuntimeError, error.what()); return nullptr; }
    catch (const std::bad_alloc&) { return PyErr_NoMemory(); }
    catch (const std::exception& error) { if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, error.what()); return nullptr; }
}

PyObject* session_cache_stats(PyObject* object, PyObject*) {
    const auto* state = reinterpret_cast<SessionObject*>(object)->state;
    const auto stats = state->cache->stats();
    return Py_BuildValue("{sK,sK,sK,sK,sK}", "entries", static_cast<unsigned long long>(stats.entries),
        "used_bytes", static_cast<unsigned long long>(stats.used_bytes),
        "hits", static_cast<unsigned long long>(stats.hits),
        "misses", static_cast<unsigned long long>(stats.misses),
        "budget_bytes", static_cast<unsigned long long>(state->cache_bytes));
}

PyObject* session_clear_cache(PyObject* object, PyObject*) {
    reinterpret_cast<SessionObject*>(object)->state->cache->clear();
    Py_RETURN_NONE;
}

PyObject* session_close(PyObject* object, PyObject*) {
    try {
        AllowThreads unlocked;
        reinterpret_cast<SessionObject*>(object)->state->close();
    } catch (const std::exception& error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return nullptr;
    }
    Py_RETURN_NONE;
}

struct RenderJobObject {
    PyObject_HEAD
    RenderJobState* state;
};

PyObject* job_new(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError, "RenderJob instances are created by session submission methods");
    return nullptr;
}

void job_dealloc(PyObject* object) {
    {
        AllowThreads unlocked;
        delete reinterpret_cast<RenderJobObject*>(object)->state;
    }
    PyTypeObject* type = Py_TYPE(object);
    type->tp_free(object);
    Py_DECREF(type);
}

PyObject* job_done(PyObject* object, PyObject*) {
    const auto* state = reinterpret_cast<RenderJobObject*>(object)->state;
    return PyBool_FromLong(state->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
}

PyObject* job_cancel(PyObject* object, PyObject*) {
    reinterpret_cast<RenderJobObject*>(object)->state->cancellation->cancel();
    Py_RETURN_NONE;
}

PyObject* job_progress(PyObject* object, PyObject*) {
    const auto& progress = reinterpret_cast<RenderJobObject*>(object)->state->progress;
    return Py_BuildValue("{sK,sK}", "completed_tiles",
        static_cast<unsigned long long>(progress->completed.load(std::memory_order_relaxed)),
        "total_tiles", static_cast<unsigned long long>(progress->total));
}

PyObject* job_result(PyObject* object, PyObject* args, PyObject* kwargs) {
    PyObject* timeout = Py_None;
    static const char* names[] = {"timeout", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", const_cast<char**>(names), &timeout)) return nullptr;
    double seconds = 0;
    if (timeout != Py_None) {
        seconds = PyFloat_AsDouble(timeout);
        if (PyErr_Occurred()) return nullptr;
        if (!std::isfinite(seconds) || seconds < 0) {
            PyErr_SetString(PyExc_ValueError, "timeout must be finite and nonnegative, or None");
            return nullptr;
        }
        const double maximum = std::chrono::duration<double>(std::chrono::steady_clock::duration::max()).count() / 2;
        if (seconds > maximum) {
            PyErr_SetString(PyExc_OverflowError, "timeout exceeds the native clock range");
            return nullptr;
        }
    }
    auto* state = reinterpret_cast<RenderJobObject*>(object)->state;
    auto* module_state = static_cast<ModuleState*>(PyType_GetModuleState(Py_TYPE(object)));
    if (!module_state) return nullptr;
    try {
        const rawengine::Tile* tile = nullptr;
        bool ready = true;
        {
            AllowThreads unlocked;
            if (timeout == Py_None) state->result.wait();
            else ready = state->result.wait_for(std::chrono::duration<double>(seconds)) == std::future_status::ready;
            if (ready) tile = &state->result.get();
        }
        if (!ready) {
            PyErr_SetString(PyExc_TimeoutError, "render job did not finish before the timeout");
            return nullptr;
        }
        if (tile->rgb.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX) / sizeof(float))
            throw std::length_error("requested ROI exceeds Python bytes capacity");
        PyObject* bytes = PyBytes_FromStringAndSize(reinterpret_cast<const char*>(tile->rgb.data()),
                                                   static_cast<Py_ssize_t>(tile->rgb.size() * sizeof(float)));
        if (!bytes) return nullptr;
        return Py_BuildValue("IIN", tile->bounds.width, tile->bounds.height, bytes);
    } catch (const rawengine::RenderCancelled& error) {
        PyErr_SetString(module_state->cancelled_error, error.what());
    } catch (const std::bad_alloc&) {
        return PyErr_NoMemory();
    } catch (const std::invalid_argument& error) {
        PyErr_SetString(PyExc_ValueError, error.what());
    } catch (const std::domain_error& error) {
        PyErr_SetString(PyExc_ValueError, error.what());
    } catch (const std::exception& error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
    }
    return nullptr;
}

PyObject* submit_job(PyObject* object, PyObject* options, const char* priority_name, PyObject* group_object,
                     PyObject* manifest = nullptr, SessionState* owned_state = nullptr,
                     const rawengine::ExecutableEditGraph* prepared_graph = nullptr) {
    rawengine::RenderPriority priority;
    const std::string_view name(priority_name);
    if (name == "background") priority = rawengine::RenderPriority::Background;
    else if (name == "normal") priority = rawengine::RenderPriority::Normal;
    else if (name == "interactive") priority = rawengine::RenderPriority::Interactive;
    else {
        PyErr_SetString(PyExc_ValueError, "priority must be background, normal or interactive");
        return nullptr;
    }
    if (options == Py_None) options = PyDict_New();
    else if (PyDict_Check(options)) Py_INCREF(options);
    else {
        PyErr_SetString(PyExc_TypeError, "options must be a dict");
        return nullptr;
    }
    if (!options) return nullptr;
    auto* state = owned_state ? owned_state : reinterpret_cast<SessionObject*>(object)->state;
    std::unique_ptr<RenderJobState, JobStateDeleter> job;
    try {
        std::string group;
        if (group_object) {
            Py_ssize_t length = 0;
            const char* text = PyUnicode_AsUTF8AndSize(group_object, &length);
            if (!text) { Py_DECREF(options); return nullptr; }
            group.assign(text, static_cast<std::size_t>(length));
            if (group.empty()) throw std::invalid_argument("latest request group must not be empty");
        }
        rawengine::RenderRequest request;
        std::optional<rawengine::ExecutableEditGraph> graph;
        if (prepared_graph ? !read_manifest_request(options, prepared_graph->output_bounds(), request)
                           : !prepare_session_graph(*state, options, manifest, request, graph)) {
            Py_DECREF(options);
            return nullptr;
        }
        Py_DECREF(options);
        options = nullptr;
        job.reset(new RenderJobState);
        {
            AllowThreads unlocked;
            state->submit(*job, prepared_graph ? *prepared_graph : *graph, request, priority, group);
        }
        auto* module_state = static_cast<ModuleState*>(PyType_GetModuleState(Py_TYPE(object)));
        if (!module_state) return nullptr;
        auto* type = reinterpret_cast<PyTypeObject*>(module_state->job_type);
        auto* result = reinterpret_cast<RenderJobObject*>(type->tp_alloc(type, 0));
        if (!result) return nullptr;
        result->state = job.release();
        return reinterpret_cast<PyObject*>(result);
    } catch (const SessionClosed& error) {
        Py_XDECREF(options);
        PyErr_SetString(PyExc_RuntimeError, error.what());
    } catch (const std::bad_alloc&) {
        Py_XDECREF(options);
        return PyErr_NoMemory();
    } catch (const std::length_error& error) {
        Py_XDECREF(options);
        PyErr_SetString(PyExc_RuntimeError, error.what());
    } catch (const std::exception& error) {
        Py_XDECREF(options);
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, error.what());
    }
    return nullptr;
}

PyObject* session_submit(PyObject* object, PyObject* args, PyObject* kwargs) {
    PyObject* options = Py_None;
    const char* priority = "normal";
    static const char* names[] = {"options", "priority", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O$s", const_cast<char**>(names), &options, &priority)) return nullptr;
    return submit_job(object, options, priority, nullptr);
}

PyObject* session_submit_latest(PyObject* object, PyObject* args, PyObject* kwargs) {
    PyObject* group = nullptr;
    PyObject* options = Py_None;
    const char* priority = "interactive";
    static const char* names[] = {"group", "options", "priority", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O$s", const_cast<char**>(names), &group, &options, &priority)) return nullptr;
    return submit_job(object, options, priority, group);
}

PyObject* session_submit_manifest(PyObject* object, PyObject* args, PyObject* kwargs) {
    PyObject *manifest = nullptr, *options = Py_None;
    const char* priority = "normal";
    static const char* names[] = {"manifest", "options", "priority", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O$s", const_cast<char**>(names),
                                     &manifest, &options, &priority)) return nullptr;
    return submit_job(object, options, priority, nullptr, manifest);
}

PyObject* session_submit_manifest_latest(PyObject* object, PyObject* args, PyObject* kwargs) {
    PyObject *group = nullptr, *manifest = nullptr, *options = Py_None;
    const char* priority = "interactive";
    static const char* names[] = {"group", "manifest", "options", "priority", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|O$s", const_cast<char**>(names),
                                     &group, &manifest, &options, &priority)) return nullptr;
    return submit_job(object, options, priority, group, manifest);
}

struct HistoryState {
    std::unique_ptr<rawengine::EditHistory> history;
    std::unique_ptr<SessionState> renders;
};
struct HistoryObject {
    PyObject_HEAD
    HistoryState* state;
};

PyObject* history_error() {
    try { throw; }
    catch (const SessionClosed& error) { PyErr_SetString(PyExc_RuntimeError, error.what()); }
    catch (const rawengine::EditRevisionUnavailable& error) { PyErr_SetString(PyExc_IndexError, error.what()); }
    catch (const std::bad_alloc&) { return PyErr_NoMemory(); }
    catch (const std::overflow_error& error) { PyErr_SetString(PyExc_OverflowError, error.what()); }
    catch (const std::exception& error) { if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, error.what()); }
    return nullptr;
}

bool copy_json(PyObject* value, std::string& json) {
    Py_ssize_t length = 0;
    const char* text = PyUnicode_AsUTF8AndSize(value, &length);
    if (!text) return false;
    json.assign(text, static_cast<std::size_t>(length));
    return true;
}

bool read_revision_id(PyObject* value, std::optional<std::uint64_t>& id) {
    if (value == Py_None) return true;
    if (!PyLong_Check(value) || PyBool_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "revision must be a positive integer ID or None");
        return false;
    }
    const auto number = PyLong_AsUnsignedLongLong(value);
    if (PyErr_Occurred()) return false;
    if (!number || number > static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())) {
        PyErr_SetString(PyExc_ValueError, "revision ID is out of range");
        return false;
    }
    id = number;
    return true;
}

rawengine::EditHistory::Snapshot history_revision(HistoryState& state, std::optional<std::uint64_t> id) {
    return id ? state.history->revision(*id) : state.history->current();
}

PyObject* session_create_history(PyObject* object, PyObject* args, PyObject* kwargs, bool restore) {
    PyObject *text = nullptr, *count_object = nullptr, *bytes_object = nullptr;
    static const char* create_names[] = {"manifest", "max_revisions", "max_manifest_bytes", nullptr};
    static const char* restore_names[] = {"saved_history", nullptr};
    if (restore) {
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", const_cast<char**>(restore_names), &text)) return nullptr;
    } else if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$OO", const_cast<char**>(create_names),
                                            &text, &count_object, &bytes_object)) return nullptr;
    try {
        std::string json;
        if (!copy_json(text, json)) return nullptr;
        rawengine::EditHistory::Limits limits;
        if (count_object) limits.max_revisions = PyLong_AsSize_t(count_object);
        if (bytes_object) limits.max_manifest_bytes = PyLong_AsSize_t(bytes_object);
        if (PyErr_Occurred()) return nullptr;
        const auto* parent = reinterpret_cast<SessionObject*>(object)->state;
        auto state = std::make_unique<HistoryState>();
        {
            AllowThreads unlocked;
            if (parent->closed.load()) throw SessionClosed();
            auto sources = parent->source_snapshot();
            if (restore)
                state->history = rawengine::EditHistory::restore(json, std::move(sources), nullptr, parent->cache);
            else
                state->history = std::make_unique<rawengine::EditHistory>(rawengine::parse_edit_manifest(json),
                    std::move(sources), limits, nullptr, parent->cache);
            state->renders = std::make_unique<SessionState>(state->history->source_bindings(),
                parent->cache_bytes, parent->workers, parent->max_pending);
            state->renders->cache = parent->cache;
        }
        auto* module_state = static_cast<ModuleState*>(PyType_GetModuleState(Py_TYPE(object)));
        if (!module_state) return nullptr;
        auto* type = reinterpret_cast<PyTypeObject*>(module_state->history_type);
        auto* result = reinterpret_cast<HistoryObject*>(type->tp_alloc(type, 0));
        if (!result) return nullptr;
        result->state = state.release();
        return reinterpret_cast<PyObject*>(result);
    } catch (...) { return history_error(); }
}

PyObject* session_history(PyObject* object, PyObject* args, PyObject* kwargs) {
    return session_create_history(object, args, kwargs, false);
}
PyObject* session_restore_history(PyObject* object, PyObject* args, PyObject* kwargs) {
    return session_create_history(object, args, kwargs, true);
}
PyObject* history_new(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_TypeError, "EditHistory instances are created by session.history or restore_history");
    return nullptr;
}
void history_dealloc(PyObject* object) {
    {
        AllowThreads unlocked;
        delete reinterpret_cast<HistoryObject*>(object)->state;
    }
    PyTypeObject* type = Py_TYPE(object);
    type->tp_free(object);
    Py_DECREF(type);
}

PyObject* history_commit(PyObject* object, PyObject* text) {
    try {
        std::string json;
        if (!copy_json(text, json)) return nullptr;
        auto* state = reinterpret_cast<HistoryObject*>(object)->state;
        std::uint64_t id;
        {
            AllowThreads unlocked;
            if (state->renders->closed.load()) throw SessionClosed();
            id = state->history->commit(rawengine::parse_edit_manifest(json));
        }
        return PyLong_FromUnsignedLongLong(id);
    } catch (...) { return history_error(); }
}
PyObject* history_navigate(PyObject* object, bool redo) {
    try {
        auto* state = reinterpret_cast<HistoryObject*>(object)->state;
        std::uint64_t id;
        {
            AllowThreads unlocked;
            if (state->renders->closed.load()) throw SessionClosed();
            id = redo ? state->history->redo() : state->history->undo();
        }
        return PyLong_FromUnsignedLongLong(id);
    } catch (...) { return history_error(); }
}
PyObject* history_undo(PyObject* object, PyObject*) { return history_navigate(object, false); }
PyObject* history_redo(PyObject* object, PyObject*) { return history_navigate(object, true); }
PyObject* history_stats(PyObject* object, PyObject*) {
    try {
        const auto stats = reinterpret_cast<HistoryObject*>(object)->state->history->stats();
        PyObject* ids = PyList_New(static_cast<Py_ssize_t>(stats.revision_ids.size()));
        if (!ids) return nullptr;
        for (std::size_t i = 0; i < stats.revision_ids.size(); ++i) {
            PyObject* id = PyLong_FromUnsignedLongLong(stats.revision_ids[i]);
            if (!id) { Py_DECREF(ids); return nullptr; }
            PyList_SET_ITEM(ids, static_cast<Py_ssize_t>(i), id);
        }
        return Py_BuildValue("{sK,sN,sK,sO,sO,sK,sK}", "current_id", static_cast<unsigned long long>(stats.current_id), "revision_ids", ids,
            "manifest_bytes", static_cast<unsigned long long>(stats.manifest_bytes),
            "can_undo", stats.can_undo ? Py_True : Py_False, "can_redo", stats.can_redo ? Py_True : Py_False,
            "max_revisions", static_cast<unsigned long long>(stats.limits.max_revisions),
            "max_manifest_bytes", static_cast<unsigned long long>(stats.limits.max_manifest_bytes));
    } catch (...) { return history_error(); }
}
PyObject* history_snapshot(PyObject* object, PyObject* args, PyObject* kwargs) {
    PyObject* revision = Py_None;
    static const char* names[] = {"revision", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", const_cast<char**>(names), &revision)) return nullptr;
    try {
        std::optional<std::uint64_t> id;
        if (!read_revision_id(revision, id)) return nullptr;
        rawengine::EditHistory::Snapshot snapshot;
        {
            AllowThreads unlocked;
            snapshot = history_revision(*reinterpret_cast<HistoryObject*>(object)->state, id);
        }
        return PyUnicode_FromStringAndSize(snapshot->manifest_json.data(), static_cast<Py_ssize_t>(snapshot->manifest_json.size()));
    } catch (...) { return history_error(); }
}
PyObject* history_save(PyObject* object, PyObject*) {
    try {
        std::string json;
        {
            AllowThreads unlocked;
            json = reinterpret_cast<HistoryObject*>(object)->state->history->serialize();
        }
        return PyUnicode_FromStringAndSize(json.data(), static_cast<Py_ssize_t>(json.size()));
    } catch (...) { return history_error(); }
}

PyObject* tile_result(const rawengine::Tile& tile) {
    if (tile.rgb.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX) / sizeof(float))
        throw std::length_error("requested ROI exceeds Python bytes capacity");
    PyObject* bytes = PyBytes_FromStringAndSize(reinterpret_cast<const char*>(tile.rgb.data()),
                                              static_cast<Py_ssize_t>(tile.rgb.size() * sizeof(float)));
    if (!bytes) return nullptr;
    return Py_BuildValue("IIN", tile.bounds.width, tile.bounds.height, bytes);
}

PyObject* history_render(PyObject* object, PyObject* args, PyObject* kwargs) {
    PyObject *options = Py_None, *revision = Py_None;
    static const char* names[] = {"options", "revision", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O$O", const_cast<char**>(names), &options, &revision)) return nullptr;
    if (options == Py_None) options = PyDict_New();
    else if (PyDict_Check(options)) Py_INCREF(options);
    else { PyErr_SetString(PyExc_TypeError, "options must be a dict"); return nullptr; }
    if (!options) return nullptr;
    try {
        std::optional<std::uint64_t> id;
        if (!read_revision_id(revision, id)) { Py_DECREF(options); return nullptr; }
        auto* state = reinterpret_cast<HistoryObject*>(object)->state;
        rawengine::EditHistory::Snapshot snapshot;
        {
            AllowThreads unlocked;
            if (state->renders->closed.load()) throw SessionClosed();
            snapshot = history_revision(*state, id);
        }
        rawengine::RenderRequest request;
        if (!read_manifest_request(options, snapshot->graph->output_bounds(), request)) { Py_DECREF(options); return nullptr; }
        Py_DECREF(options); options = nullptr;
        rawengine::Tile result;
        {
            AllowThreads unlocked;
            result = rawengine::Renderer{}.render_image(*snapshot->graph, request);
        }
        return tile_result(result);
    } catch (...) { Py_XDECREF(options); return history_error(); }
}

PyObject* history_submit_common(PyObject* object, PyObject* args, PyObject* kwargs, bool latest) {
    PyObject *options = Py_None, *revision = Py_None, *group = nullptr;
    const char* priority = latest ? "interactive" : "normal";
    static const char* names[] = {"options", "revision", "priority", nullptr};
    static const char* latest_names[] = {"group", "options", "revision", "priority", nullptr};
    if (latest) {
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O$Os", const_cast<char**>(latest_names),
                &group, &options, &revision, &priority)) return nullptr;
    } else if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O$Os", const_cast<char**>(names),
                                            &options, &revision, &priority)) return nullptr;
    try {
        std::optional<std::uint64_t> id;
        if (!read_revision_id(revision, id)) return nullptr;
        auto* state = reinterpret_cast<HistoryObject*>(object)->state;
        rawengine::EditHistory::Snapshot snapshot;
        {
            AllowThreads unlocked;
            if (state->renders->closed.load()) throw SessionClosed();
            snapshot = history_revision(*state, id);
        }
        return submit_job(object, options, priority, group, nullptr, state->renders.get(), snapshot->graph.get());
    } catch (...) { return history_error(); }
}
PyObject* history_submit(PyObject* object, PyObject* args, PyObject* kwargs) { return history_submit_common(object, args, kwargs, false); }
PyObject* history_submit_latest(PyObject* object, PyObject* args, PyObject* kwargs) { return history_submit_common(object, args, kwargs, true); }

PyObject* history_compare(PyObject* object, PyObject* args, PyObject* kwargs) {
    PyObject *first = nullptr, *second = nullptr, *options = Py_None;
    static const char* names[] = {"first", "second", "options", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|O", const_cast<char**>(names), &first, &second, &options)) return nullptr;
    if (options == Py_None) options = PyDict_New();
    else if (PyDict_Check(options)) Py_INCREF(options);
    else { PyErr_SetString(PyExc_TypeError, "options must be a dict"); return nullptr; }
    if (!options) return nullptr;
    try {
        std::optional<std::uint64_t> a, b;
        if (!read_revision_id(first, a) || !read_revision_id(second, b)) { Py_DECREF(options); return nullptr; }
        if (!a || !b) { Py_DECREF(options); PyErr_SetString(PyExc_TypeError, "compare requires two explicit revision IDs"); return nullptr; }
        auto* state = reinterpret_cast<HistoryObject*>(object)->state;
        rawengine::EditHistory::Snapshot left, right;
        {
            AllowThreads unlocked;
            if (state->renders->closed.load()) throw SessionClosed();
            std::tie(left, right) = state->history->comparison(*a, *b);
        }
        rawengine::RenderRequest left_request, right_request;
        if (!read_manifest_request(options, left->graph->output_bounds(), left_request) ||
            !read_manifest_request(options, right->graph->output_bounds(), right_request)) { Py_DECREF(options); return nullptr; }
        Py_DECREF(options); options = nullptr;
        rawengine::Tile left_tile, right_tile;
        {
            AllowThreads unlocked;
            left_tile = rawengine::Renderer{}.render_image(*left->graph, left_request);
            right_tile = rawengine::Renderer{}.render_image(*right->graph, right_request);
        }
        PyObject* left_result = tile_result(left_tile);
        if (!left_result) return nullptr;
        PyObject* right_result = tile_result(right_tile);
        if (!right_result) { Py_DECREF(left_result); return nullptr; }
        return Py_BuildValue("NN", left_result, right_result);
    } catch (...) { Py_XDECREF(options); return history_error(); }
}

PyObject* history_close(PyObject* object, PyObject*) {
    try {
        AllowThreads unlocked;
        reinterpret_cast<HistoryObject*>(object)->state->renders->close();
    } catch (...) { return history_error(); }
    Py_RETURN_NONE;
}

PyMethodDef history_methods[] = {
    {"commit", history_commit, METH_O, "commit(manifest) -> new revision ID. Validate before publishing; discard redo and evict oldest states to meet budgets."},
    {"undo", history_undo, METH_NOARGS, "Select the previous revision or raise IndexError."},
    {"redo", history_redo, METH_NOARGS, "Select the next retained revision or raise IndexError."},
    {"stats", history_stats, METH_NOARGS, "Return current/revision IDs, retained manifest bytes, navigation flags and limits."},
    {"snapshot", reinterpret_cast<PyCFunction>(history_snapshot), METH_VARARGS | METH_KEYWORDS, "snapshot(revision=None) -> canonical manifest JSON."},
    {"save", history_save, METH_NOARGS, "Return deterministic history-format-1 JSON; sources are referenced by fingerprint, not stored as pixels."},
    {"render", reinterpret_cast<PyCFunction>(history_render), METH_VARARGS | METH_KEYWORDS, "render(options=None, *, revision=None) -> RGB result for a pinned retained revision."},
    {"submit", reinterpret_cast<PyCFunction>(history_submit), METH_VARARGS | METH_KEYWORDS, "submit(options=None, *, revision=None, priority='normal') -> RenderJob."},
    {"submit_latest", reinterpret_cast<PyCFunction>(history_submit_latest), METH_VARARGS | METH_KEYWORDS, "submit_latest(group, options=None, *, revision=None, priority='interactive') -> RenderJob."},
    {"compare", reinterpret_cast<PyCFunction>(history_compare), METH_VARARGS | METH_KEYWORDS, "compare(first, second, options=None) -> (first_result, second_result). Geometry defaults are independent."},
    {"close", history_close, METH_NOARGS, "Reject new mutations/renders/jobs and cancel unfinished jobs; stats/snapshot/save remain readable."},
    {nullptr, nullptr, 0, nullptr}
};
PyType_Slot history_slots[] = {
    {Py_tp_new, reinterpret_cast<void*>(history_new)},
    {Py_tp_dealloc, reinterpret_cast<void*>(history_dealloc)},
    {Py_tp_methods, history_methods},
    {Py_tp_doc, const_cast<char*>("Immutable manifest revision history with pinned source snapshots, bounded retention and shared tile cache. Created by session.history/restore_history.")},
    {0, nullptr}
};
PyType_Spec history_spec = {"rawengine_native.EditHistory", sizeof(HistoryObject), 0, Py_TPFLAGS_DEFAULT, history_slots};

PyMethodDef job_methods[] = {
    {"result", reinterpret_cast<PyCFunction>(job_result), METH_VARARGS | METH_KEYWORDS,
     "result(timeout=None) -> (width, height, float32_rgb_bytes). Releases GIL while waiting; repeatable results."},
    {"done", job_done, METH_NOARGS, "True when a result or exception is ready."},
    {"cancel", job_cancel, METH_NOARGS, "Request cancellation at a tile boundary; completed results remain available."},
    {"progress", job_progress, METH_NOARGS, "Return completed_tiles and total_tiles; no intra-tile progress or ETA."},
    {nullptr, nullptr, 0, nullptr}
};
PyType_Slot job_slots[] = {
    {Py_tp_new, reinterpret_cast<void*>(job_new)},
    {Py_tp_dealloc, reinterpret_cast<void*>(job_dealloc)},
    {Py_tp_methods, job_methods},
    {Py_tp_doc, const_cast<char*>("An owned asynchronous render. Dropping an unfinished job requests cancellation; retained jobs retain native result memory.")},
    {0, nullptr}
};
PyType_Spec job_spec = {"rawengine_native.RenderJob", sizeof(RenderJobObject), 0,
                       Py_TPFLAGS_DEFAULT, job_slots};

PyMethodDef session_methods[] = {
    {"history", reinterpret_cast<PyCFunction>(session_history), METH_VARARGS | METH_KEYWORDS,
     "history(manifest, *, max_revisions=64, max_manifest_bytes=4194304) -> EditHistory. Pin declared source snapshots and share the tile cache."},
    {"restore_history", reinterpret_cast<PyCFunction>(session_restore_history), METH_VARARGS | METH_KEYWORDS,
     "restore_history(saved_history) -> EditHistory. Validate all saved states against this session's current owned sources."},
    {"required_source_regions", reinterpret_cast<PyCFunction>(session_required_regions), METH_VARARGS | METH_KEYWORDS,
     "required_source_regions(manifest, options=None) -> {source_id: (x,y,width,height)}. Native source footprints for the requested output ROI/level."},
    {"source_info", session_source_info, METH_NOARGS,
     "Return a copy of the owned source identity/fingerprint and native dimensions; available after close."},
    {"export_manifest", reinterpret_cast<PyCFunction>(session_export_manifest), METH_VARARGS | METH_KEYWORDS,
     "export_manifest(options=None) -> JSON str. Snapshot a recipe as a format-v2 graph; render-request options are not saved."},
    {"render_manifest", reinterpret_cast<PyCFunction>(session_render_manifest), METH_VARARGS | METH_KEYWORDS,
     "render_manifest(manifest, options=None) -> (width, height, float32_rgb_bytes). Execute saved JSON against this session's verified source."},
    {"submit_manifest", reinterpret_cast<PyCFunction>(session_submit_manifest), METH_VARARGS | METH_KEYWORDS,
     "submit_manifest(manifest, options=None, *, priority='normal') -> RenderJob."},
    {"submit_manifest_latest", reinterpret_cast<PyCFunction>(session_submit_manifest_latest), METH_VARARGS | METH_KEYWORDS,
     "submit_manifest_latest(group, manifest, options=None, *, priority='interactive') -> RenderJob. Shares groups with recipe jobs."},
    {"render", reinterpret_cast<PyCFunction>(session_render), METH_VARARGS | METH_KEYWORDS,
     "render(options=None) -> (width, height, float32_rgb_bytes). Complete per-call recipe, mip and quality; source metadata is fixed."},
    {"submit", reinterpret_cast<PyCFunction>(session_submit), METH_VARARGS | METH_KEYWORDS,
     "submit(options=None, *, priority='normal') -> RenderJob."},
    {"submit_latest", reinterpret_cast<PyCFunction>(session_submit_latest), METH_VARARGS | METH_KEYWORDS,
     "submit_latest(group, options=None, *, priority='interactive') -> RenderJob. Supersedes queued/running jobs in this session's group."},
    {"close", session_close, METH_NOARGS, "Reject new renders/submissions and cancel unfinished submitted jobs. Idempotent."},
    {"cache_stats", session_cache_stats, METH_NOARGS, "Return entries, used_bytes, hits, misses and budget_bytes."},
    {"clear_cache", session_clear_cache, METH_NOARGS, "Clear cached tiles and reset hit/miss counters."},
    {nullptr, nullptr, 0, nullptr}
};

PyType_Slot session_slots[] = {
    {Py_tp_new, reinterpret_cast<void*>(session_new)},
    {Py_tp_dealloc, reinterpret_cast<void*>(session_dealloc)},
    {Py_tp_methods, session_methods},
    {Py_tp_doc, const_cast<char*>("RasterSession(rgb, width, height, working_space, row_stride_pixels=0, cache_bytes=67108864, workers=1, max_pending=8). Owns source/cache; scheduler starts lazily. Each request supplies a complete recipe.")},
    {0, nullptr}
};
PyType_Spec session_spec = {"rawengine_native.RasterSession", sizeof(SessionObject), 0,
                           Py_TPFLAGS_DEFAULT, session_slots};

PyType_Slot raw_session_slots[] = {
    {Py_tp_new, reinterpret_cast<void*>(raw_session_new)},
    {Py_tp_dealloc, reinterpret_cast<void*>(session_dealloc)},
    {Py_tp_methods, session_methods},
    {Py_tp_doc, const_cast<char*>("RawSession(bayer, width, height, metadata=None, *, cache_bytes=67108864, workers=1, max_pending=8, demosaic=None). Owns decoded uint16 Bayer, fixed sensor metadata and versioned demosaic policy. None pins rawengine.bilinear processing_version 1; explicit policy is a dict with algorithm and processing_version. Recipes export format 3. Each recipe supplies WB/calibration/exposure/tone; reduced preview is active-area-relative, native final is sensor-relative.")},
    {0, nullptr}
};
PyType_Spec raw_session_spec = {"rawengine_native.RawSession", sizeof(SessionObject), 0,
                               Py_TPFLAGS_DEFAULT, raw_session_slots};

PyMethodDef graph_session_methods[] = {
    {"history", reinterpret_cast<PyCFunction>(session_history), METH_VARARGS | METH_KEYWORDS,
     "history(manifest, *, max_revisions=64, max_manifest_bytes=4194304) -> EditHistory."},
    {"restore_history", reinterpret_cast<PyCFunction>(session_restore_history), METH_VARARGS | METH_KEYWORDS,
     "restore_history(saved_history) -> EditHistory. Saved source fingerprints must match current owned sources."},
    {"source_info", graph_session_source_info, METH_NOARGS, "Return source-ID to copied identity/dimensions metadata; available after close."},
    {"export_manifest", reinterpret_cast<PyCFunction>(graph_session_export_manifest), METH_VARARGS | METH_KEYWORDS,
     "export_manifest(source_id) -> format-v2 JSON. All owned source records, no edits; selected source is output."},
    {"replace_source", reinterpret_cast<PyCFunction>(graph_session_replace_source), METH_VARARGS | METH_KEYWORDS,
     "replace_source(source_id, source) -> None. Atomically publish a validated copy; existing jobs retain their source snapshot."},
    {"render_manifest", reinterpret_cast<PyCFunction>(session_render_manifest), METH_VARARGS | METH_KEYWORDS,
     "render_manifest(manifest, options=None) -> (width,height,float32_rgb_bytes). Bind exactly the saved source records from owned sources."},
    {"required_source_regions", reinterpret_cast<PyCFunction>(session_required_regions), METH_VARARGS | METH_KEYWORDS,
     "required_source_regions(manifest, options=None) -> {source_id: (x,y,width,height)}."},
    {"submit_manifest", reinterpret_cast<PyCFunction>(session_submit_manifest), METH_VARARGS | METH_KEYWORDS,
     "submit_manifest(manifest, options=None, *, priority='normal') -> RenderJob."},
    {"submit_manifest_latest", reinterpret_cast<PyCFunction>(session_submit_manifest_latest), METH_VARARGS | METH_KEYWORDS,
     "submit_manifest_latest(group, manifest, options=None, *, priority='interactive') -> RenderJob."},
    {"close", session_close, METH_NOARGS, "Reject new work and cancel unfinished jobs. Idempotent."},
    {"cache_stats", session_cache_stats, METH_NOARGS, "Return entries, used_bytes, hits, misses and budget_bytes."},
    {"clear_cache", session_clear_cache, METH_NOARGS, "Clear cached tiles and reset counters."},
    {nullptr, nullptr, 0, nullptr}
};
PyType_Slot graph_session_slots[] = {
    {Py_tp_new, reinterpret_cast<void*>(graph_session_new)},
    {Py_tp_dealloc, reinterpret_cast<void*>(session_dealloc)},
    {Py_tp_methods, graph_session_methods},
    {Py_tp_doc, const_cast<char*>("RasterGraphSession(sources, *, cache_bytes=67108864, workers=1, max_pending=8). Owns 1-64 scene-linear sources keyed by stable lowercase UUIDs; executes saved manifests.")},
    {0, nullptr}
};
PyType_Spec graph_session_spec = {"rawengine_native.RasterGraphSession", sizeof(SessionObject), 0,
                                 Py_TPFLAGS_DEFAULT, graph_session_slots};

PyMethodDef methods[] = {
    {"render", reinterpret_cast<PyCFunction>(render), METH_VARARGS | METH_KEYWORDS,
     "render(bayer, width, height, options=None) -> (width, height, float32_rgb_bytes)\n"
     "Input: contiguous native-endian uint16 Bayer buffer. Pattern: 0=RGGB, 1=BGGR, 2=GRBG, 3=GBRG. "
     "Mip 1/2 preview requires camera calibration and srgb-preview output; preview ROI is active-area-relative. "
     "Options: x, y, roi_width, roi_height, black_level, "
     "white_level, black_levels, white_levels, pattern, row_stride_samples, "
     "cfa_phase_x, cfa_phase_y, active_x, active_y, active_width, active_height, "
     "tile_size, red_gain, green_gain, blue_gain, "
     "exposure_stops, tone_shoulder, tone_gamma, camera_to_xyz_d50 (nine "
     "row-major doubles), working_space (prophoto-d50 or rec2020-d65), "
     "output_mode (legacy or srgb-preview), mip (0/1/2), quality (final/preview)."},
    {"render_raster", reinterpret_cast<PyCFunction>(render_raster),
     METH_VARARGS | METH_KEYWORDS,
     "render_raster(rgb, width, height, options) -> (width, height, float32_rgb_bytes)\n"
     "Input: contiguous native-endian, scene-linear interleaved float32 RGB. "
     "Options: working_space (required: prophoto-d50 or rec2020-d65), "
     "row_stride_pixels, x, y, roi_width, roi_height, tile_size, "
     "exposure_stops, tone_shoulder, tone_gamma, output_mode "
     "(legacy or srgb-preview), mip (0/1/2; default 0), quality (final/preview; "
     "default final). Mip 1/2 requires preview and srgb-preview output. "
     "crop=(x,y,width,height) uses original-source pixels and precedes edits/reduction. "
     "resize=(width,height) follows crop; resize_filter is nearest, bilinear or area. "
     "rotate=0/90/180/270 is clockwise; bool flips follow rotation, before resize. "
     "ROI and tile dimensions use mip pixels; default ROI is the full mip image."},
    {nullptr, nullptr, 0, nullptr}
};

int module_traverse(PyObject* object, visitproc visit, void* arg) {
    auto* state = static_cast<ModuleState*>(PyModule_GetState(object));
    Py_VISIT(state->job_type);
    Py_VISIT(state->history_type);
    Py_VISIT(state->cancelled_error);
    return 0;
}

int module_clear(PyObject* object) {
    auto* state = static_cast<ModuleState*>(PyModule_GetState(object));
    Py_CLEAR(state->job_type);
    Py_CLEAR(state->history_type);
    Py_CLEAR(state->cancelled_error);
    return 0;
}

void module_free(void* object) { module_clear(static_cast<PyObject*>(object)); }

PyModuleDef module = {PyModuleDef_HEAD_INIT, "rawengine_native",
                      "Tiled, non-destructive RAW rendering.", sizeof(ModuleState), methods,
                      nullptr, module_traverse, module_clear, module_free};

} // namespace

PyMODINIT_FUNC PyInit_rawengine_native() {
    PyObject* result = PyModule_Create(&module);
    if (!result) return nullptr;
    auto* state = static_cast<ModuleState*>(PyModule_GetState(result));
    state->job_type = PyType_FromModuleAndSpec(result, &job_spec, nullptr);
    state->history_type = PyType_FromModuleAndSpec(result, &history_spec, nullptr);
    state->cancelled_error = PyErr_NewException("rawengine_native.RenderCancelled", PyExc_RuntimeError, nullptr);
    if (!state->job_type || !state->history_type || !state->cancelled_error) { Py_DECREF(result); return nullptr; }
    Py_INCREF(state->job_type);
    if (PyModule_AddObject(result, "RenderJob", state->job_type) < 0) {
        Py_DECREF(state->job_type); Py_DECREF(result); return nullptr;
    }
    Py_INCREF(state->history_type);
    if (PyModule_AddObject(result, "EditHistory", state->history_type) < 0) {
        Py_DECREF(state->history_type); Py_DECREF(result); return nullptr;
    }
    Py_INCREF(state->cancelled_error);
    if (PyModule_AddObject(result, "RenderCancelled", state->cancelled_error) < 0) {
        Py_DECREF(state->cancelled_error); Py_DECREF(result); return nullptr;
    }
    PyObject* session_type = PyType_FromModuleAndSpec(result, &session_spec, nullptr);
    if (!session_type || PyModule_AddObject(result, "RasterSession", session_type) < 0) {
        Py_XDECREF(session_type);
        Py_DECREF(result);
        return nullptr;
    }
    PyObject* graph_session_type = PyType_FromModuleAndSpec(result, &graph_session_spec, nullptr);
    if (!graph_session_type || PyModule_AddObject(result, "RasterGraphSession", graph_session_type) < 0) {
        Py_XDECREF(graph_session_type);
        Py_DECREF(result);
        return nullptr;
    }
    PyObject* raw_session_type = PyType_FromModuleAndSpec(result, &raw_session_spec, nullptr);
    if (!raw_session_type || PyModule_AddObject(result, "RawSession", raw_session_type) < 0) {
        Py_XDECREF(raw_session_type);
        Py_DECREF(result);
        return nullptr;
    }
    return result;
}
