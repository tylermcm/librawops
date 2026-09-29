#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "RawEngine.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

struct BufferGuard {
    Py_buffer view{};
    ~BufferGuard() { if (view.obj) PyBuffer_Release(&view); }
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
        const bool explicit_roi = PyDict_GetItemString(options, "x") ||
                                  PyDict_GetItemString(options, "y") ||
                                  PyDict_GetItemString(options, "roi_width") ||
                                  PyDict_GetItemString(options, "roi_height");
        const bool valid =
            read_uint(options, "x", roi.x) && read_uint(options, "y", roi.y) &&
            read_uint(options, "roi_width", roi.width) &&
            read_uint(options, "roi_height", roi.height) &&
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
            read_float(options, "tone_gamma", recipe.tone_gamma);
        if (!valid) { Py_DECREF(options); return nullptr; }
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
        Py_DECREF(options);
        options = nullptr;
        if (!explicit_roi && active_area.width && active_area.height)
            roi = active_area;
        const auto actual_stride = stride ? stride : width;
        const auto count = static_cast<std::uint64_t>(actual_stride) * height;
        if (!width || !height || count > std::numeric_limits<std::size_t>::max() / 2 ||
            count > static_cast<std::uint64_t>(PY_SSIZE_T_MAX) / 2 ||
            buffer.view.len != static_cast<Py_ssize_t>(count * 2))
            throw std::invalid_argument("bayer must contain row_stride_samples*height native-endian uint16 samples");
        std::vector<std::uint16_t> samples(static_cast<std::size_t>(count));
        std::memcpy(samples.data(), buffer.view.buf, static_cast<std::size_t>(count) * 2);
        rawengine::RawImage raw(metadata, std::move(samples));
        rawengine::ImageGraph graph(std::move(raw), recipe);
        auto pixels = rawengine::Renderer{}.render_roi(graph, roi, tile_size);
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

PyMethodDef methods[] = {
    {"render", reinterpret_cast<PyCFunction>(render), METH_VARARGS | METH_KEYWORDS,
     "render(bayer, width, height, options=None) -> (width, height, float32_rgb_bytes)\n"
     "Input: contiguous native-endian uint16 Bayer buffer. Pattern: 0=RGGB, 1=BGGR, "
     "2=GRBG, 3=GBRG. Options: x, y, roi_width, roi_height, black_level, "
     "white_level, black_levels, white_levels, pattern, row_stride_samples, "
     "cfa_phase_x, cfa_phase_y, active_x, active_y, active_width, active_height, "
     "tile_size, red_gain, green_gain, blue_gain, "
     "exposure_stops, tone_shoulder, tone_gamma."},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef module = {PyModuleDef_HEAD_INIT, "rawengine_native",
                      "Tiled, non-destructive RAW rendering.", -1, methods,
                      nullptr, nullptr, nullptr, nullptr};

} // namespace

PyMODINIT_FUNC PyInit_rawengine_native() { return PyModule_Create(&module); }
