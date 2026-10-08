cmake_minimum_required(VERSION 3.16)
project(supabase-cpp VERSION 0.5.0 LANGUAGES CXX) # x-release-please-version

option(SUPABASE_BUILD_TESTS "Build supabase-cpp tests" OFF)
option(SUPABASE_BUILD_EXAMPLES "Build supabase-cpp examples" OFF)

if(NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

file(GLOB_RECURSE SUPABASE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(supabase STATIC ${SUPABASE_SOURCES})
add_library(supabase::supabase ALIAS supabase)

target_compile_features(supabase PUBLIC cxx_std_20)
target_include_directories(supabase PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(supabase PUBLIC nlohmann_json::nlohmann_json PRIVATE CURL::libcurl)

if(SUPABASE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SUPABASE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
