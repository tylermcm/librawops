cmake_minimum_required(VERSION 3.20)
project(LibRawOpsMenonResearch LANGUAGES CXX)

# Standalone research only: no connection to the product build or install rules.
add_executable(menon_research menon_research.cpp)
target_compile_features(menon_research PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(menon_research PRIVATE /W4 /fp:strict)
else()
    target_compile_options(menon_research PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()
