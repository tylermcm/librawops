cmake_minimum_required(VERSION 3.18)
project(RawEngine VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

add_library(RawEngine SHARED RawEngine.cpp)
add_library(RawEngine::RawEngine ALIAS RawEngine)
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
    add_executable(RawEngineCoreTests tests/core_tests.cpp)
    target_link_libraries(RawEngineCoreTests PRIVATE RawEngine::RawEngine)
    add_test(NAME RawEngineCoreTests COMMAND RawEngineCoreTests)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME RawEngineReferenceHarness
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_harness_tests.py)
    endif()
    if(RAWENGINE_WITH_LCMS)
        add_executable(RawEngineLittleCmsTests tests/lcms_tests.cpp)
        target_link_libraries(RawEngineLittleCmsTests PRIVATE RawEngine::RawEngine lcms2)
        add_test(NAME RawEngineLittleCmsTests COMMAND RawEngineLittleCmsTests)
    endif()
endif()

option(RAWENGINE_BUILD_BENCHMARK "Build the repeatable native render benchmark" OFF)
if(RAWENGINE_BUILD_BENCHMARK)
    add_executable(RawEngineBenchmark bench/render_benchmark.cpp)
    target_link_libraries(RawEngineBenchmark PRIVATE RawEngine::RawEngine)
    add_executable(RawEngineColorBenchmark bench/color_space_benchmark.cpp)
    target_link_libraries(RawEngineColorBenchmark PRIVATE RawEngine::RawEngine)
endif()

option(RAWENGINE_BUILD_PYTHON "Build the optional CPython extension" ON)
if(RAWENGINE_BUILD_PYTHON)
    find_package(Python3 QUIET COMPONENTS Interpreter Development.Module)
    if(Python3_Development.Module_FOUND)
        Python3_add_library(rawengine_native MODULE WITH_SOABI RawEnginePython.cpp)
        target_compile_features(rawengine_native PRIVATE cxx_std_20)
        target_link_libraries(rawengine_native PRIVATE RawEngine::RawEngine)
        if(BUILD_TESTING)
            add_test(NAME RawEnginePythonSmoke
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.py
                             $<TARGET_FILE_DIR:rawengine_native>)
        endif()
    else()
        message(STATUS "Python development headers unavailable; skipping rawengine_native")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS RawEngine EXPORT RawEngineTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES RawEngine.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION share/licenses/RawEngine RENAME LibRawOps-LICENSE)
if(RAWENGINE_WITH_LCMS)
    install(FILES LittleCmsBackend.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
if(TARGET rawengine_native)
    install(TARGETS rawengine_native LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(EXPORT RawEngineTargets NAMESPACE RawEngine::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RawEngine)
