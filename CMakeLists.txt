cmake_minimum_required(VERSION 3.21)

project(LinkQualityFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral runtime for evidence-backed link signal, error, degradation and quality state"
  HOMEPAGE_URL "https://github.com/summonlabs/Link-Quality-Fabric"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Single source of truth for on-disk and on-wire format versions.
# ---------------------------------------------------------------------------
set(LQF_JOURNAL_FORMAT_VERSION 1)
set(LQF_WIRE_PROTOCOL_VERSION 1)

option(LQF_BUILD_APPS           "Build the lqf command line runtime"          ON)
option(LQF_BUILD_TESTS          "Build the Link Quality Fabric test suites"  ON)
option(LQF_BUILD_BENCHMARKS     "Build the Link Quality Fabric benchmarks"   ON)
option(LQF_BUILD_EXAMPLES       "Build the Link Quality Fabric examples"     ON)
option(LQF_WARNINGS_AS_ERRORS   "Treat first-party warnings as errors"       ON)
option(LQF_ENABLE_ANALYZE       "Enable MSVC static analysis (/analyze)"     OFF)
option(LQF_LOCK_TRACKING        "Enable runtime lock order and re-entrancy auditing" ON)
option(LQF_SANITIZE             "Sanitizer to enable (empty, address, undefined)" "")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

include(LinkQualityFabricWarnings)

# ---------------------------------------------------------------------------
# Generated public version header
# ---------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/lqf/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# The runtime library
# ---------------------------------------------------------------------------
set(LQF_SOURCES
  src/core/status.cpp
  src/core/text.cpp
  src/core/hash.cpp
  src/core/clock.cpp
  src/core/lock_tracker.cpp
  src/domain/quality.cpp
  src/domain/units.cpp
  src/domain/metric.cpp
  src/domain/identity.cpp
  src/domain/observation.cpp
  src/domain/capability.cpp
  src/domain/policy.cpp
  src/domain/classification.cpp
  src/domain/counter.cpp
  src/store/capability_registry.cpp
  src/store/evidence_store.cpp
  src/store/link_registry.cpp
  src/classify/classifier.cpp
  src/classify/explain.cpp
  src/persist/codec.cpp
  src/persist/journal.cpp
  src/runtime/config.cpp
  src/runtime/fabric.cpp
  src/transport/frame.cpp
  src/transport/socket.cpp
  src/transport/server.cpp
  src/transport/client.cpp
)

add_library(lqf ${LQF_SOURCES})
add_library(lqf::lqf ALIAS lqf)

target_include_directories(lqf
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(lqf
  PUBLIC
    $<$<BOOL:${BUILD_SHARED_LIBS}>:LQF_SHARED=1>
    $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${WIN32}>:NOMINMAX>)
target_compile_definitions(lqf PRIVATE LQF_BUILDING_LIBRARY=1)

target_link_libraries(lqf PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(lqf PUBLIC ws2_32)
endif()

# Lock order / re-entrancy auditing. Enabled by default and in Debug builds. It
# is a PUBLIC definition because LockScope and TrackedMutex are header visible
# and must have one definition across the library and its consumers.
if(LQF_LOCK_TRACKING)
  target_compile_definitions(lqf PUBLIC LQF_LOCK_TRACKING=1)
endif()

lqf_apply_warnings(lqf)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(LQF_BUILD_APPS)
  add_executable(lqf_cli
    apps/lqf/main.cpp
    apps/lqf/cmd_common.cpp
    apps/lqf/cmd_serve.cpp
    apps/lqf/cmd_journal.cpp
    apps/lqf/cmd_client.cpp
    apps/lqf/cmd_generate.cpp)
  set_target_properties(lqf_cli PROPERTIES OUTPUT_NAME lqf)
  target_include_directories(lqf_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/lqf")
  target_link_libraries(lqf_cli PRIVATE lqf::lqf)
  lqf_apply_warnings(lqf_cli)
endif()

# ---------------------------------------------------------------------------
# Tests, benchmarks, examples
# ---------------------------------------------------------------------------
if(LQF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LQF_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(LQF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS lqf
  EXPORT LinkQualityFabricTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LQF_BUILD_APPS)
  install(TARGETS lqf_cli
    EXPORT LinkQualityFabricTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(DIRECTORY include/lqf
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/lqf/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lqf")

install(EXPORT LinkQualityFabricTargets
  FILE LinkQualityFabricTargets.cmake
  NAMESPACE lqf::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LinkQualityFabric")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LinkQualityFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/LinkQualityFabricConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LinkQualityFabric")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LinkQualityFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/LinkQualityFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LinkQualityFabricConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LinkQualityFabric")

install(FILES LICENSE NOTICE README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/LinkQualityFabric")
