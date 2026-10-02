cmake_minimum_required(VERSION 3.18)
project(RawEngine VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

add_library(RawEngine SHARED RawEngine.cpp MenonDemosaic.cpp EditGraph.cpp Sha256.cpp SourceFingerprint.cpp TileScheduler.cpp ImageAnalysis.cpp SpatialOps.cpp ToneOps.cpp)
add_library(RawEngine::RawEngine ALIAS RawEngine)

# Menon v1 and point tone/color edits preserve float64 order without contraction.
if(MSVC)
    set_source_files_properties(MenonDemosaic.cpp ToneOps.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
else()
    set_source_files_properties(MenonDemosaic.cpp ToneOps.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
target_compile_features(RawEngine PUBLIC cxx_std_20)
target_compile_definitions(RawEngine PRIVATE RAWENGINE_BUILDING)
target_include_directories(RawEngine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)
set_target_properties(RawEngine PROPERTIES CXX_VISIBILITY_PRESET default)

option(RAWENGINE_USE_OPENMP "Use OpenMP for large tiles when available" ON)
if(RAWENGINE_USE_OPENMP)
    find_package(OpenMP QUIET COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(RawEngine PRIVATE OpenMP::OpenMP_CXX)
    endif()
endif()

option(RAWENGINE_WITH_LCMS "Build the optional core-only LittleCMS ICC backend" OFF)
if(RAWENGINE_WITH_LCMS)
    enable_language(C)
    include(FetchContent)
    FetchContent_Declare(lcms2
        URL https://codeload.github.com/mm2/Little-CMS/tar.gz/refs/tags/lcms2.19.1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        URL_HASH SHA256=267705e278e2f7c2fb886c259dadcbaeb2be52748bcbc71c79f08aacacb7a709)
    set(LCMS2_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(LCMS2_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(LCMS2_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LCMS2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LCMS2_WITH_FASTFLOAT OFF CACHE BOOL "" FORCE)
    set(LCMS2_WITH_THREADED_PLUGIN OFF CACHE BOOL "" FORCE)
    set(LCMS2_WITH_JPEG OFF CACHE BOOL "" FORCE)
    set(LCMS2_WITH_TIFF OFF CACHE BOOL "" FORCE)
    set(LCMS2_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    set(LCMS2_WITH_THREADS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(lcms2)
    if(TARGET lcms2_fast_float OR TARGET lcms2_threaded)
        message(FATAL_ERROR "GPL LittleCMS plugin target was unexpectedly enabled")
    endif()
    set_target_properties(lcms2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_sources(RawEngine PRIVATE LittleCmsBackend.cpp)
    target_link_libraries(RawEngine PRIVATE lcms2)
    install(FILES ${lcms2_SOURCE_DIR}/LICENSE
            DESTINATION share/licenses/RawEngine RENAME LittleCMS-LICENSE)
endif()

if(BUILD_TESTING)
    add_executable(RawEngineGrayscaleTests tests/grayscale_tests.cpp)
    target_link_libraries(RawEngineGrayscaleTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineGrayscaleTests COMMAND RawEngineGrayscaleTests)
    add_executable(RawEngineColorBalanceTests tests/color_balance_tests.cpp)
    target_link_libraries(RawEngineColorBalanceTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineColorBalanceTests COMMAND RawEngineColorBalanceTests)
    add_executable(RawEngineColorMixerTests tests/color_mixer_tests.cpp)
    target_link_libraries(RawEngineColorMixerTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineColorMixerTests COMMAND RawEngineColorMixerTests)
    add_executable(RawEngineLut3DTests tests/lut3d_tests.cpp)
    target_link_libraries(RawEngineLut3DTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineLut3DTests COMMAND RawEngineLut3DTests)
    add_executable(RawEngineLut1DTests tests/lut1d_tests.cpp)
    target_link_libraries(RawEngineLut1DTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineLut1DTests COMMAND RawEngineLut1DTests)
    add_executable(RawEngineChannelMixerTests tests/channel_mixer_tests.cpp)
    target_link_libraries(RawEngineChannelMixerTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineChannelMixerTests COMMAND RawEngineChannelMixerTests)
    add_executable(RawEngineVibranceTests tests/vibrance_tests.cpp)
    target_link_libraries(RawEngineVibranceTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineVibranceTests COMMAND RawEngineVibranceTests)
    add_executable(RawEngineSaturationTests tests/saturation_tests.cpp)
    target_link_libraries(RawEngineSaturationTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineSaturationTests COMMAND RawEngineSaturationTests)
    add_executable(RawEngineToneOpsTests tests/tone_ops_tests.cpp)
    target_link_libraries(RawEngineToneOpsTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineToneOpsTests COMMAND RawEngineToneOpsTests)
    add_executable(RawEngineSpatialOpsTests tests/spatial_ops_tests.cpp)
    target_link_libraries(RawEngineSpatialOpsTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineSpatialOpsTests COMMAND RawEngineSpatialOpsTests)
    add_executable(RawEngineImageAnalysisTests tests/image_analysis_tests.cpp)
    target_link_libraries(RawEngineImageAnalysisTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineImageAnalysisTests COMMAND RawEngineImageAnalysisTests)
    add_executable(RawEngineCoreTests tests/core_tests.cpp)
    target_link_libraries(RawEngineCoreTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineCoreTests COMMAND RawEngineCoreTests)
    add_executable(RawEngineEditGraphTests tests/edit_graph_tests.cpp)
    target_link_libraries(RawEngineEditGraphTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineEditGraphTests COMMAND RawEngineEditGraphTests)
    add_executable(RawEngineEditHistoryTests tests/edit_history_tests.cpp)
    target_link_libraries(RawEngineEditHistoryTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineEditHistoryTests COMMAND RawEngineEditHistoryTests)
    add_executable(RawEngineRawPreviewTests tests/raw_preview_tests.cpp)
    target_link_libraries(RawEngineRawPreviewTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineRawPreviewTests COMMAND RawEngineRawPreviewTests)
    add_executable(RawEngineRawDemosaicTests tests/raw_demosaic_tests.cpp)
    target_link_libraries(RawEngineRawDemosaicTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineRawDemosaicTests COMMAND RawEngineRawDemosaicTests)
    add_executable(RawEngineRawMenonNativeTests tests/raw_menon_native_tests.cpp)
    target_link_libraries(RawEngineRawMenonNativeTests PRIVATE RawEngine::RawEngine)
    target_compile_definitions(RawEngineRawMenonNativeTests PRIVATE
        RAWENGINE_MENON_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/raw/menon_native_base_v1.bin")
    add_test(NAME RawEngineRawMenonNativeTests COMMAND RawEngineRawMenonNativeTests)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME RawEngineReferenceHarness
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_harness_tests.py)
        add_test(NAME RawEngineRawQualityHarness
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_quality_harness_tests.py)
        add_test(NAME RawEngineRawQualityCompare
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_quality_compare_tests.py)
        add_test(NAME RawEngineRawCameraCorpus
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_camera_corpus_tests.py)
        add_test(NAME RawEngineRawHaReference
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_ha_reference_tests.py)
        add_test(NAME RawEngineRawQuantizationAudit
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_quantization_audit_tests.py)
        set_tests_properties(RawEngineRawQuantizationAudit PROPERTIES TIMEOUT 120)
        add_test(NAME RawEngineRawMenonSupport
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_menon_support_tests.py)
        set_tests_properties(RawEngineRawMenonSupport PROPERTIES TIMEOUT 120)
        add_test(NAME RawEngineRawMenonReference
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_menon_reference_tests.py)
        set_tests_properties(RawEngineRawMenonReference PROPERTIES TIMEOUT 120)
        add_test(NAME RawEngineRawMenonDiagnostics
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_menon_diagnostics_tests.py)
        set_tests_properties(RawEngineRawMenonDiagnostics PROPERTIES TIMEOUT 30)
        add_test(NAME RawEngineRawMenonGuard
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_menon_guard_tests.py)
        set_tests_properties(RawEngineRawMenonGuard PROPERTIES TIMEOUT 120)
    endif()
    if(RAWENGINE_WITH_LCMS)
        add_executable(RawEngineLittleCmsTests tests/lcms_tests.cpp)
        target_link_libraries(RawEngineLittleCmsTests PRIVATE RawEngine::RawEngine lcms2)
        target_compile_definitions(RawEngineLittleCmsTests PRIVATE
            RAWENGINE_TEST_PROFILE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/profiles/sRGB2014.icc")
        add_test(NAME RawEngineLittleCmsTests COMMAND RawEngineLittleCmsTests)
    endif()
endif()

option(RAWENGINE_BUILD_BENCHMARK "Build the repeatable native render benchmark" OFF)
if(RAWENGINE_BUILD_BENCHMARK)
    add_executable(RawEngineMenonBenchmark bench/raw_menon_benchmark.cpp)
    target_link_libraries(RawEngineMenonBenchmark PRIVATE RawEngine::RawEngine)
    add_executable(RawEngineBenchmark bench/render_benchmark.cpp)
    target_link_libraries(RawEngineBenchmark PRIVATE RawEngine::RawEngine)
    add_executable(RawEngineColorBenchmark bench/color_space_benchmark.cpp)
    target_link_libraries(RawEngineColorBenchmark PRIVATE RawEngine::RawEngine)
    add_executable(RawEngineCacheBenchmark bench/cache_benchmark.cpp)
    target_link_libraries(RawEngineCacheBenchmark PRIVATE RawEngine::RawEngine)
    if(RAWENGINE_WITH_LCMS)
        add_executable(RawEngineIccBenchmark bench/icc_benchmark.cpp)
        target_link_libraries(RawEngineIccBenchmark PRIVATE RawEngine::RawEngine)
        target_compile_definitions(RawEngineIccBenchmark PRIVATE
            RAWENGINE_TEST_PROFILE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/profiles/sRGB2014.icc")
        if(WIN32)
            target_link_libraries(RawEngineIccBenchmark PRIVATE psapi)
        endif()
    endif()
endif()

option(RAWENGINE_BUILD_PYTHON "Build the optional CPython extension" ON)
if(RAWENGINE_BUILD_PYTHON)
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter Development.Module)
    if(Python3_Development.Module_FOUND)
        Python3_add_library(rawengine_native MODULE WITH_SOABI RawEnginePython.cpp)
        target_compile_features(rawengine_native PRIVATE cxx_std_20)
        target_link_libraries(rawengine_native PRIVATE RawEngine::RawEngine)
        if(BUILD_TESTING)
            add_test(NAME RawEnginePythonGrayscaleTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_grayscale_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonColorBalanceTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_color_balance_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonColorMixerTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_color_mixer_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonLut3DTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_lut3d_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonLut1DTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_lut1d_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonChannelMixerTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_channel_mixer_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonVibranceTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_vibrance_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonSaturationTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_saturation_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonToneOpsTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_tone_ops_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonSpatialOpsTests
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_spatial_ops_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonImageAnalysisTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_image_analysis_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonSmoke
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonPreviewTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_preview_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonAsyncTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_async_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonManifestTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_manifest_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            set_tests_properties(RawEnginePythonAsyncTests PROPERTIES TIMEOUT 30)
            set_tests_properties(RawEnginePythonManifestTests PROPERTIES TIMEOUT 30)
            add_test(NAME RawEnginePythonMultiSourceTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_multi_source_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            set_tests_properties(RawEnginePythonMultiSourceTests PROPERTIES TIMEOUT 30)
            add_test(NAME RawEnginePythonHistoryTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_history_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            add_test(NAME RawEnginePythonRawPreviewTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_raw_preview_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            set_tests_properties(RawEnginePythonRawPreviewTests PROPERTIES TIMEOUT 30)
            add_test(NAME RawEnginePythonRawSessionTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_raw_session_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            set_tests_properties(RawEnginePythonRawSessionTests PROPERTIES TIMEOUT 30)
            add_test(NAME RawEnginePythonRawMenonTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_raw_menon_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            set_tests_properties(RawEnginePythonRawMenonTests PROPERTIES TIMEOUT 30)
            add_test(NAME RawEnginePythonRawQualityTests
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_raw_quality_tests.py
                             $<TARGET_FILE_DIR:rawengine_native>)
            set_tests_properties(RawEnginePythonRawQualityTests PROPERTIES TIMEOUT 30)
            set_tests_properties(RawEnginePythonHistoryTests PROPERTIES TIMEOUT 30)
        endif()
    else()
        message(STATUS "Python 3.9+ development headers unavailable; skipping rawengine_native")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS RawEngine EXPORT RawEngineTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES RawEngine.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ImageAnalysis.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES SpatialOps.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ToneOps.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES EditGraph.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES TileScheduler.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION share/licenses/RawEngine RENAME LibRawOps-LICENSE)
install(FILES third_party/colour-demosaicing/LICENSE
        DESTINATION share/licenses/RawEngine RENAME Colour-Demosaicing-LICENSE)
if(RAWENGINE_WITH_LCMS)
    install(FILES LittleCmsBackend.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
if(TARGET rawengine_native)
    install(TARGETS rawengine_native LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(EXPORT RawEngineTargets NAMESPACE RawEngine::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RawEngine)
