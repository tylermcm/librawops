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

PyObject* render(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* source = nullptr;
    PyObject* options = Py_None;
    unsigned int width = 0, height = 0;
    static const char* names[] = {"bayer", "width", "height", "options", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OII|O", const_cast<char**>(names),
                                     &source, &width, &height, &options)) return nullptr;
    if (options == Py_None) options = PyDict_New();
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
        const auto count = static_cast<std::uint64_t>(width) * height;
        if (!width || !height || count > std::numeric_limits<std::size_t>::max() / 2 ||
            count > static_cast<std::uint64_t>(PY_SSIZE_T_MAX) / 2 ||
            buffer.view.len != static_cast<Py_ssize_t>(count * 2))
            throw std::invalid_argument("bayer must contain width*height native-endian uint16 samples");
        std::vector<std::uint16_t> samples(static_cast<std::size_t>(count));
        std::memcpy(samples.data(), buffer.view.buf, static_cast<std::size_t>(count) * 2);

        std::uint32_t black = 0, white = 65535, pattern = 0, tile_size = 256;
        rawengine::Rect roi{0, 0, width, height};
        rawengine::GraphRecipe recipe;
        const bool valid =
            read_uint(options, "x", roi.x) && read_uint(options, "y", roi.y) &&
            read_uint(options, "roi_width", roi.width) &&
            read_uint(options, "roi_height", roi.height) &&
            read_uint(options, "black_level", black) &&
            read_uint(options, "white_level", white) &&
            read_uint(options, "pattern", pattern) &&
            read_uint(options, "tile_size", tile_size) &&
            read_float(options, "red_gain", recipe.red_gain) &&
            read_float(options, "green_gain", recipe.green_gain) &&
            read_float(options, "blue_gain", recipe.blue_gain) &&
            read_float(options, "exposure_stops", recipe.exposure_stops) &&
            read_float(options, "tone_shoulder", recipe.tone_shoulder) &&
            read_float(options, "tone_gamma", recipe.tone_gamma);
        Py_DECREF(options);
        options = nullptr;
        if (!valid) return nullptr;
        if (black > 65535 || white > 65535 || pattern > 3)
            throw std::invalid_argument("invalid levels or Bayer pattern");

        rawengine::RawImage raw(width, height, std::move(samples),
                                static_cast<rawengine::BayerPattern>(pattern),
                                static_cast<std::uint16_t>(black),
                                static_cast<std::uint16_t>(white));
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
     "white_level, pattern, tile_size, red_gain, green_gain, blue_gain, "
     "exposure_stops, tone_shoulder, tone_gamma."},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef module = {PyModuleDef_HEAD_INIT, "rawengine_native",
                      "Tiled, non-destructive RAW rendering.", -1, methods,
                      nullptr, nullptr, nullptr, nullptr};

} // namespace

PyMODINIT_FUNC PyInit_rawengine_native() { return PyModule_Create(&module); }
