cmake_minimum_required(VERSION 3.24)
# The lowest macOS the binaries run on (14.2: Core Audio Process Taps, the
# system sound capture). Must come before project(): there
# CMake creates the cache entry (empty, i.e. "this machine's macOS") and a
# later set(... CACHE ...) no longer changes it. -DCMAKE_OSX_DEPLOYMENT_TARGET
# of 14.2 or later, or the MACOSX_DEPLOYMENT_TARGET environment variable,
# still win; anything lower (a build directory configured when the default
# was 12.3 or 14.0 keeps that in its cache) is raised to 14.2.
if(NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET} AND
   (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 14.2))
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.2" CACHE STRING "Minimum macOS version" FORCE)
endif()
project(rdaccel VERSION 3.4.0 LANGUAGES C CXX)
# Pre-release label ("rc1"), empty for a release. The full version, as the
# library, the packages and the tag (v1.0.0-rc1) show it, is
# PROJECT_VERSION[-RDACCEL_PRERELEASE].
set(RDACCEL_PRERELEASE "")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Tests and examples default to on only when rdaccel is the top-level project,
# so add_subdirectory(rdaccel) in another project builds just the library.
option(RDACCEL_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(RDACCEL_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
option(RDACCEL_BUILD_TOOLS "Build development tools (tools/clip)" ${PROJECT_IS_TOP_LEVEL})
option(RDACCEL_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
# Also install rdaccel_serve / rdaccel_view / rdaccel_record / rdaccel_play
# (release packages do).
option(RDACCEL_INSTALL_TOOLS "Install the example command-line tools" OFF)
# libFuzzer targets for every parser of network data (clang only). Configure
# with CMAKE_C_FLAGS/CMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined";
# see fuzz/README.md.
option(RDACCEL_BUILD_FUZZERS "Build libFuzzer targets" OFF)
# Sound (1.13): Opus, fetched at configure time (the first dependency of the
# library itself that is downloaded; set FETCHCONTENT_SOURCE_DIR_OPUS to a
# copy of the sources to build offline). OFF: the sound functions return
# RDA_ERR_UNSUPPORTED.
option(RDACCEL_AUDIO "Sound (Opus)" ON)

include(GNUInstallDirs)

# One version: the header's RDA_VERSION_* must match project(VERSION).
file(STRINGS include/rdaccel/rdaccel.h RDA_VERSION_LINES REGEX "^#define RDA_VERSION_(MAJOR|MINOR|PATCH) ")
string(REGEX REPLACE ".*MAJOR ([0-9]+).*MINOR ([0-9]+).*PATCH ([0-9]+).*" "\\1.\\2.\\3"
       RDA_HEADER_VERSION "${RDA_VERSION_LINES}")
if(NOT RDA_HEADER_VERSION STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR "rdaccel.h says ${RDA_HEADER_VERSION}, CMakeLists.txt says ${PROJECT_VERSION}")
endif()
file(STRINGS include/rdaccel/rdaccel.h RDA_PRERELEASE_LINE REGEX "^#define RDA_VERSION_PRERELEASE ")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" RDA_HEADER_PRERELEASE "${RDA_PRERELEASE_LINE}")
if(NOT RDA_HEADER_PRERELEASE STREQUAL RDACCEL_PRERELEASE)
  message(FATAL_ERROR "rdaccel.h says pre-release \"${RDA_HEADER_PRERELEASE}\", CMakeLists.txt says \"${RDACCEL_PRERELEASE}\"")
endif()
if(RDACCEL_PRERELEASE)
  set(RDACCEL_FULL_VERSION "${PROJECT_VERSION}-${RDACCEL_PRERELEASE}")
else()
  set(RDACCEL_FULL_VERSION "${PROJECT_VERSION}")
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

# macOS only since 2.0 (Linux support was removed).
if(NOT APPLE)
  message(FATAL_ERROR "rdaccel supports macOS only")
endif()
enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)

add_compile_options(-Wall -Wextra)
# An API newer than CMAKE_OSX_DEPLOYMENT_TARGET must sit behind
# @available / __builtin_available, or it crashes on older macOS.
add_compile_options(-Werror=unguarded-availability-new)

# ---------------------------------------------------------------------------
# The library: one target, rdaccel::rdaccel.
#
#   include/   public API: rdaccel.h (C ABI) + rdaccel.hpp (header-only C++)
#   src/capi/  the C ABI, on top of the internal C++ API
#   src/       internal C++ API (Host, Viewer, transports) in namespace rdacore
#   core/      platform-independent internals
#   platform/  capture, input and the HEVC codec on macOS
# ---------------------------------------------------------------------------
set(RDACCEL_SOURCES
  src/capi/capi.cpp
  src/host.cpp
  src/viewer.cpp
  src/transport.cpp
  src/misc.cpp
  src/keycodes.cpp
  core/proto/wire.cpp
  core/proto/messages.cpp
  core/tile/tile_diff.cpp
  core/tile/motion.cpp
  core/tile/tile_cache.cpp
  core/tile/video_region.cpp
  core/codec/hevc_bitstream.cpp
  core/codec/lossless.cpp
  core/codec/context_tiles.cpp
  core/capture/synthetic_capturer.cpp
  core/transport/local_pipe.cpp
  core/transport/secure_transport.cpp
  third_party/monocypher/monocypher.c
  core/net/sim_link.cpp
  core/net/udp_socket.cpp
  core/net/reliable_transport.cpp
  core/net/fec.cpp
  core/session/host_session.cpp
  core/session/link.cpp
  core/session/viewer_session.cpp
  core/session/rate_control.cpp
  core/session/cursor_channel.cpp
  core/io/stream_file.cpp
  core/log/log.cpp
)
if(RDACCEL_AUDIO)
  list(APPEND RDACCEL_SOURCES
    core/audio/audio_packet.cpp
    core/audio/sender.cpp
    core/audio/receiver.cpp
    core/audio/resampler.cpp
    core/audio/test_signal.cpp
    core/audio/player.cpp
  )
  list(APPEND RDACCEL_SOURCES platform/mac/audio_tap.mm platform/mac/audio_player.mm)
endif()
list(APPEND RDACCEL_SOURCES
  platform/mac/platform_mac.mm
  platform/mac/capture_sck.mm
  platform/mac/displays_mac.mm
  platform/mac/cursor_mac.mm
  platform/mac/inject_mac.mm
  platform/mac/video_vt.mm
)

# Before 1.0 a minor version may change the ABI, so it is part of the soname.
if(PROJECT_VERSION_MAJOR EQUAL 0)
  set(RDACCEL_SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
else()
  set(RDACCEL_SOVERSION ${PROJECT_VERSION_MAJOR})
endif()
add_library(rdaccel ${RDACCEL_SOURCES})
add_library(rdaccel::rdaccel ALIAS rdaccel)
# Only the rda_* functions are exported from a shared build.
set_target_properties(rdaccel PROPERTIES
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  OBJCXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  EXPORT_NAME rdaccel
  VERSION ${PROJECT_VERSION}
  SOVERSION ${RDACCEL_SOVERSION})
target_compile_features(rdaccel PUBLIC cxx_std_20)
get_target_property(RDACCEL_TYPE rdaccel TYPE)
if(RDACCEL_TYPE STREQUAL "SHARED_LIBRARY")
  target_link_options(rdaccel PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/rdaccel.exp")
endif()
target_include_directories(rdaccel
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher)
target_compile_definitions(rdaccel PRIVATE RDACCEL_VERSION_STRING="${RDACCEL_FULL_VERSION}")
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(rdaccel PRIVATE Threads::Threads ZLIB::ZLIB)

if(RDACCEL_AUDIO)
  # Opus (BSD licence, NOTICE), static, inside the library: a shared
  # librdaccel exports none of its symbols (cmake/rdaccel.exp); a
  # static one installs libopus.a next to itself.
  # Intrinsics off: Opus's CMake picks the SSE or NEON files for the one CPU
  # it configures for, so a universal build (arm64 and x86_64 in one
  # configure) would compile one architecture's intrinsics into the other.
  # Plain C is fast enough (encoding runs at over 50 times real time; see
  # tests/test_audio_codec.cpp).
  include(FetchContent)
  set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
  FetchContent_Declare(opus
    URL https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
    URL_HASH SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  # Opus as a static library even where rdaccel is shared.
  set(RDA_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_MakeAvailable(opus)
  set(BUILD_SHARED_LIBS ${RDA_SAVED_BUILD_SHARED_LIBS})
  set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
  target_compile_options(opus PRIVATE -w)  # its own warnings are not ours
  target_link_libraries(rdaccel PRIVATE $<BUILD_INTERFACE:opus>)
  target_compile_definitions(rdaccel PRIVATE RDACCEL_HAVE_AUDIO=1)
else()
  target_compile_definitions(rdaccel PRIVATE RDACCEL_HAVE_AUDIO=0)
endif()

target_compile_options(rdaccel PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(rdaccel PRIVATE
  "-framework Foundation"
  "-framework AppKit"
  "-framework Carbon"
  "-framework CoreGraphics"
  "-framework CoreMedia"
  "-framework CoreVideo"
  "-framework VideoToolbox"
  "-framework ScreenCaptureKit"
  "-framework IOSurface"
  "-framework IOKit"
  "-framework CoreAudio"
  "-framework AudioToolbox"
)

# Internal headers, for the library's own unit tests only. Not installed;
# applications use include/rdaccel/ alone.
add_library(rdaccel_internal INTERFACE)
target_link_libraries(rdaccel_internal INTERFACE rdaccel Threads::Threads)
target_include_directories(rdaccel_internal INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/core ${CMAKE_CURRENT_SOURCE_DIR}/platform
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher)
if(RDACCEL_AUDIO)
  # The tests call Opus themselves (the codec test), and build against the
  # internal headers, which the RDACCEL_HAVE_AUDIO switch shapes.
  target_link_libraries(rdaccel_internal INTERFACE opus)
  target_compile_definitions(rdaccel_internal INTERFACE RDACCEL_HAVE_AUDIO=1)
else()
  target_compile_definitions(rdaccel_internal INTERFACE RDACCEL_HAVE_AUDIO=0)
endif()

# ---------------------------------------------------------------------------
# install + find_package(rdaccel)
# ---------------------------------------------------------------------------
if(RDACCEL_INSTALL)
  include(CMakePackageConfigHelpers)
  install(TARGETS rdaccel EXPORT rdaccelTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/rdaccel DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT rdaccelTargets
    NAMESPACE rdaccel::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rdaccel)
  if(RDACCEL_TYPE STREQUAL "STATIC_LIBRARY")
    set(RDACCEL_STATIC ON)
  else()
    set(RDACCEL_STATIC OFF)
  endif()
  configure_package_config_file(cmake/rdaccelConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rdaccelConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rdaccel)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/rdaccelConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rdaccelConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rdaccelConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rdaccel)

  # pkg-config, relocatable (paths relative to the .pc file). A static
  # library also needs its private dependencies: `pkg-config --static`.
  file(RELATIVE_PATH RDA_PC_TO_PREFIX "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
  string(REGEX REPLACE "/$" "" RDA_PC_TO_PREFIX "${RDA_PC_TO_PREFIX}")
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(RDA_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  else()
    set(RDA_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(RDA_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
  else()
    set(RDA_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  if(RDACCEL_AUDIO)
    set(RDA_PC_OPUS "-lopus ")
  else()
    set(RDA_PC_OPUS "")
  endif()
  set(RDA_PC_LIBS_PRIVATE "${RDA_PC_OPUS}-lz -lc++ -framework Foundation -framework AppKit -framework Carbon -framework CoreGraphics -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework ScreenCaptureKit -framework IOSurface -framework IOKit -framework CoreAudio -framework AudioToolbox")
  configure_file(cmake/rdaccel.pc.in ${CMAKE_CURRENT_BINARY_DIR}/rdaccel.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rdaccel.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  install(FILES LICENSE NOTICE README.md CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

  # `cpack` (or `cmake --build build --target package`): a .tar.gz of the
  # install tree, named rdaccel-<version>-<os>-<arch>[-static]. A macOS
  # build for both arm64 and x86_64 is "universal".
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" RDA_OS)
  set(RDA_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
  if(CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES RDA_ARCH_COUNT)
    if(RDA_ARCH_COUNT GREATER 1)
      set(RDA_ARCH universal)
    else()
      set(RDA_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    endif()
  endif()
  string(TOLOWER "${RDA_ARCH}" RDA_ARCH)
  if(RDACCEL_STATIC)
    string(APPEND RDA_ARCH "-static")
  endif()
  set(CPACK_GENERATOR TGZ)
  set(CPACK_PACKAGE_NAME rdaccel)
  set(CPACK_PACKAGE_VENDOR rdaccel)
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Low-latency remote desktop streaming library (HEVC)")
  set(CPACK_PACKAGE_FILE_NAME "rdaccel-${RDACCEL_FULL_VERSION}-${RDA_OS}-${RDA_ARCH}")
  set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
  set(CPACK_SOURCE_GENERATOR TGZ)
  set(CPACK_SOURCE_PACKAGE_FILE_NAME "rdaccel-${RDACCEL_FULL_VERSION}")
  set(CPACK_SOURCE_IGNORE_FILES "/[.]git/" "/build[^/]*/" "/inst[^/]*/" "^${CMAKE_CURRENT_SOURCE_DIR}/corpus/" "/crashes/")
  include(CPack)
endif()

# ---------------------------------------------------------------------------
# examples: programs built on the public API only
# ---------------------------------------------------------------------------
if(RDACCEL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(RDACCEL_BUILD_TESTS)
  # Prefer an installed GoogleTest (e.g. `brew install googletest`), else
  # fetch it with git.
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2
      GIT_SHALLOW TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(rdaccel_tests
    tests/test_wire.cpp
    tests/test_messages.cpp
    tests/test_tile_diff.cpp
    tests/test_dirty_rects.cpp
    tests/test_motion.cpp
    tests/test_tile_cache.cpp
    tests/test_video_region.cpp
    tests/test_hevc_bitstream.cpp
    tests/test_lossless.cpp
    tests/test_local_pipe.cpp
    tests/test_reliable_transport.cpp
    tests/test_fec.cpp
    tests/test_session.cpp
    tests/test_cursor.cpp
    tests/test_link.cpp
    tests/test_keycodes.cpp
    tests/test_secure_transport.cpp
    tests/test_text_cells.cpp
    tests/test_text_cell.cpp
  )
  # text_cells.h is a header of the clip tool (tools/clip), not of the library.
  set_source_files_properties(tests/test_text_cells.cpp PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/tools/clip)
  if(RDACCEL_AUDIO)
    target_sources(rdaccel_tests PRIVATE
      tests/test_audio_packet.cpp
      tests/test_audio_gate.cpp
      tests/test_audio_codec.cpp
      tests/test_audio_resampler.cpp
      tests/test_audio_jitter.cpp)
  endif()
  target_link_libraries(rdaccel_tests PRIVATE rdaccel_internal GTest::gtest_main)
  include(GoogleTest)
  # Every test runs with the sound player's null device forced on: nothing
  # a test does can open a real output device (docs/integration.md, sound).
  gtest_discover_tests(rdaccel_tests PROPERTIES ENVIRONMENT "RDACCEL_AUDIO_NULL_OUTPUT=1")

  # Platform codec round trip: encode synthetic frames with the platform
  # encoder and decode them with the platform decoder.
  # The public API tests use only include/rdaccel/: test_api.cpp the C++
  # wrapper (rdaccel.hpp), test_capi.cpp the C ABI (rdaccel.h).
  add_executable(rdaccel_platform_tests tests/test_platform_codec.cpp tests/test_api.cpp
    tests/test_capi.cpp)
  target_link_libraries(rdaccel_platform_tests PRIVATE rdaccel_internal GTest::gtest_main)
  if(RDACCEL_AUDIO)
    target_sources(rdaccel_platform_tests PRIVATE tests/test_platform_audio_mac.mm)
    target_compile_options(rdaccel_platform_tests PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(rdaccel_platform_tests PRIVATE "-framework CoreAudio")
  endif()
  gtest_discover_tests(rdaccel_platform_tests DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT "RDACCEL_AUDIO_NULL_OUTPUT=1")

  # The plain C example doubles as a test of the C ABI from C.
  if(RDACCEL_BUILD_EXAMPLES)
    add_test(NAME minimal_c COMMAND rdaccel_minimal_c)
    set_tests_properties(minimal_c PROPERTIES
      ENVIRONMENT "RDACCEL_AUDIO_NULL_OUTPUT=1")
  endif()
endif()

# ---------------------------------------------------------------------------
# tools: development tools on the internals (not installed)
# ---------------------------------------------------------------------------
if(RDACCEL_BUILD_TOOLS)
  # Test clips of a screen, and how the host does on them (docs/clips.md).
  add_executable(rdaccel_clip tools/clip/rdaccel_clip.cpp)
  target_include_directories(rdaccel_clip PRIVATE tools/clip examples)
  target_link_libraries(rdaccel_clip PRIVATE rdaccel_internal ZLIB::ZLIB)
endif()

if(RDACCEL_BUILD_FUZZERS)
  set(RDA_FUZZERS messages lossless viewer link secure reliable hevc recording)
  if(RDACCEL_AUDIO)
    list(APPEND RDA_FUZZERS audio)
  endif()
  foreach(name ${RDA_FUZZERS})
    add_executable(fuzz_${name} fuzz/fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE rdaccel_internal)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(fuzz_${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz)
  endforeach()
endif()
