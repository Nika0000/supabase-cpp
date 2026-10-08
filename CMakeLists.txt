cmake_minimum_required(VERSION 3.16)
project(supabase-cpp VERSION 0.5.0 LANGUAGES CXX) # x-release-please-version

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SUPABASE_BUILD_TESTS "Build supabase-cpp tests" OFF)
option(SUPABASE_BUILD_INTEGRATION_TESTS "Build live-stack integration tests" OFF)
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
target_include_directories(supabase PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(supabase PUBLIC nlohmann_json::nlohmann_json PRIVATE CURL::libcurl)

install(TARGETS supabase EXPORT supabaseTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT supabaseTargets NAMESPACE supabase:: FILE supabaseTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/supabase)
configure_package_config_file(cmake/supabaseConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/supabaseConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/supabase)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/supabaseConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/supabaseConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/supabaseConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/supabase)

if(SUPABASE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SUPABASE_BUILD_INTEGRATION_TESTS)
    add_subdirectory(tests/integration)
endif()

if(SUPABASE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
