cmake_minimum_required(VERSION 3.24)
project(rdaccel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RDACCEL_BUILD_TESTS "Build unit tests" ON)

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# core: platform-independent code. Must build and test on Linux and macOS.
# ---------------------------------------------------------------------------
add_library(rdaccel_core STATIC
  core/proto/wire.cpp
  core/proto/messages.cpp
  core/tile/tile_diff.cpp
  core/codec/hevc_bitstream.cpp
  core/capture/synthetic_capturer.cpp
  core/transport/local_pipe.cpp
  core/transport/tcp_transport.cpp
  core/session/host_session.cpp
  core/session/viewer_session.cpp
  core/io/stream_file.cpp
)
target_include_directories(rdaccel_core PUBLIC include core)
find_package(Threads REQUIRED)
target_link_libraries(rdaccel_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# platform: capture + HEVC codecs. One implementation per OS behind
# platform/platform.h.
# ---------------------------------------------------------------------------
if(APPLE)
  add_library(rdaccel_platform STATIC
    platform/mac/platform_mac.mm
    platform/mac/capture_sck.mm
    platform/mac/video_vt.mm
  )
  target_compile_options(rdaccel_platform PRIVATE -fobjc-arc)
  target_link_libraries(rdaccel_platform PUBLIC
    rdaccel_core
    "-framework Foundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework ScreenCaptureKit"
    "-framework IOSurface"
  )
elseif(UNIX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
  pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xext)
  add_library(rdaccel_platform STATIC
    platform/linux/platform_linux.cpp
    platform/linux/capture_x11.cpp
    platform/linux/video_ffmpeg.cpp
  )
  target_link_libraries(rdaccel_platform PUBLIC rdaccel_core PkgConfig::FFMPEG PkgConfig::X11)
endif()
target_include_directories(rdaccel_platform PUBLIC platform)

# ---------------------------------------------------------------------------
# tools (both platforms)
# ---------------------------------------------------------------------------
add_executable(rdaccel_record tools/record.cpp)
target_link_libraries(rdaccel_record PRIVATE rdaccel_platform)
add_executable(rdaccel_play tools/play.cpp)
target_link_libraries(rdaccel_play PRIVATE rdaccel_platform)

# macOS: single-machine loopback viewer with a window.
if(APPLE)
  add_executable(rdaccel_loopback MACOSX_BUNDLE apps/loopback/main.mm)
  target_compile_options(rdaccel_loopback PRIVATE -fobjc-arc)
  target_link_libraries(rdaccel_loopback PRIVATE
    rdaccel_platform
    "-framework AppKit"
    "-framework QuartzCore"
  )
  set_target_properties(rdaccel_loopback PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/apps/loopback/Info.plist.in)
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
    tests/test_hevc_bitstream.cpp
    tests/test_local_pipe.cpp
    tests/test_tcp_transport.cpp
    tests/test_session.cpp
  )
  target_link_libraries(rdaccel_tests PRIVATE rdaccel_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rdaccel_tests)

  # Platform codec round trip: encode synthetic frames with the platform
  # encoder and decode them with the platform decoder.
  add_executable(rdaccel_platform_tests tests/test_platform_codec.cpp)
  target_link_libraries(rdaccel_platform_tests PRIVATE rdaccel_platform GTest::gtest_main)
  gtest_discover_tests(rdaccel_platform_tests DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
endif()
