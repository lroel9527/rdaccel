cmake_minimum_required(VERSION 3.24)
project(rdaccel VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Tests and examples default to on only when rdaccel is the top-level project,
# so add_subdirectory(rdaccel) in another project builds just the library.
option(RDACCEL_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(RDACCEL_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
option(RDACCEL_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
# Also install rdaccel_serve / rdaccel_view / rdaccel_record / rdaccel_play
# (release packages do).
option(RDACCEL_INSTALL_TOOLS "Install the example command-line tools" OFF)
# libFuzzer targets for every parser of network data (clang only). Configure
# with CMAKE_C_FLAGS/CMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined";
# see fuzz/README.md.
option(RDACCEL_BUILD_FUZZERS "Build libFuzzer targets" OFF)

include(GNUInstallDirs)

# One version: the header's RDA_VERSION_* must match project(VERSION).
file(STRINGS include/rdaccel/rdaccel.h RDA_VERSION_LINES REGEX "^#define RDA_VERSION_(MAJOR|MINOR|PATCH) ")
string(REGEX REPLACE ".*MAJOR ([0-9]+).*MINOR ([0-9]+).*PATCH ([0-9]+).*" "\\1.\\2.\\3"
       RDA_HEADER_VERSION "${RDA_VERSION_LINES}")
if(NOT RDA_HEADER_VERSION STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR "rdaccel.h says ${RDA_HEADER_VERSION}, CMakeLists.txt says ${PROJECT_VERSION}")
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

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
# The library: one target, rdaccel::rdaccel.
#
#   include/   public API: rdaccel.h (C ABI) + rdaccel.hpp (header-only C++)
#   src/capi/  the C ABI, on top of the internal C++ API
#   src/       internal C++ API (Host, Viewer, transports) in namespace rdacore
#   core/      platform-independent internals
#   platform/  capture + HEVC codecs, one implementation per OS
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
  core/capture/synthetic_capturer.cpp
  core/transport/local_pipe.cpp
  core/transport/tcp_transport.cpp
  core/transport/secure_transport.cpp
  third_party/monocypher/monocypher.c
  core/net/sim_link.cpp
  core/net/udp_socket.cpp
  core/net/reliable_transport.cpp
  core/session/host_session.cpp
  core/session/link.cpp
  core/session/viewer_session.cpp
  core/session/rate_control.cpp
  core/session/cursor_channel.cpp
  core/io/stream_file.cpp
  core/log/log.cpp
)
if(APPLE)
  list(APPEND RDACCEL_SOURCES
    platform/mac/platform_mac.mm
    platform/mac/capture_sck.mm
    platform/mac/displays_mac.mm
    platform/mac/cursor_mac.mm
    platform/mac/video_vt.mm
  )
elseif(UNIX)
  list(APPEND RDACCEL_SOURCES
    platform/linux/platform_linux.cpp
    platform/linux/capture_x11.cpp
    platform/linux/cursor_xfixes.cpp
    platform/linux/displays_x11.cpp
    platform/linux/video_ffmpeg.cpp
  )
else()
  message(FATAL_ERROR "rdaccel supports macOS and Linux only")
endif()

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
  if(APPLE)
    target_link_options(rdaccel PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/rdaccel.exp")
  else()
    target_link_options(rdaccel PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/rdaccel.map")
  endif()
endif()
target_include_directories(rdaccel
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher)
target_compile_definitions(rdaccel PRIVATE RDACCEL_VERSION_STRING="${PROJECT_VERSION}")
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(rdaccel PRIVATE Threads::Threads ZLIB::ZLIB)

if(APPLE)
  target_compile_options(rdaccel PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(rdaccel PRIVATE
    "-framework Foundation"
    "-framework AppKit"
    "-framework CoreGraphics"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework ScreenCaptureKit"
    "-framework IOSurface"
  )
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
  pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xext xfixes xrandr)
  target_link_libraries(rdaccel PRIVATE PkgConfig::FFMPEG PkgConfig::X11)
endif()

# Internal headers, for the library's own unit tests only. Not installed;
# applications use include/rdaccel/ alone.
add_library(rdaccel_internal INTERFACE)
target_link_libraries(rdaccel_internal INTERFACE rdaccel Threads::Threads)
target_include_directories(rdaccel_internal INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/core ${CMAKE_CURRENT_SOURCE_DIR}/platform
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher)

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
    COMPATIBILITY SameMinorVersion)
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
  # Private dependencies are listed as plain flags (not Requires.private) so
  # `pkg-config --static` does not expand FFmpeg's own static dependencies:
  # rdaccel links the shared FFmpeg/X11 libraries, as the CMake package does.
  if(APPLE)
    set(RDA_PC_LIBS_PRIVATE "-lz -lc++ -framework Foundation -framework AppKit -framework CoreGraphics -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework ScreenCaptureKit -framework IOSurface")
  else()
    string(JOIN " " RDA_PC_LIBS_PRIVATE ${FFMPEG_LDFLAGS} ${X11_LDFLAGS} -lz -lstdc++ -lm -pthread)
  endif()
  configure_file(cmake/rdaccel.pc.in ${CMAKE_CURRENT_BINARY_DIR}/rdaccel.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rdaccel.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  install(FILES LICENSE NOTICE README.md CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

  # `cpack` (or `cmake --build build --target package`): a .tar.gz of the
  # install tree, named rdaccel-<version>-<os>-<arch>.
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" RDA_OS)
  set(RDA_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
  if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    string(REPLACE ";" "-" RDA_ARCH "${CMAKE_OSX_ARCHITECTURES}")
  endif()
  string(TOLOWER "${RDA_ARCH}" RDA_ARCH)
  set(CPACK_GENERATOR TGZ)
  set(CPACK_PACKAGE_NAME rdaccel)
  set(CPACK_PACKAGE_VENDOR rdaccel)
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Low-latency remote desktop streaming library (HEVC)")
  set(CPACK_PACKAGE_FILE_NAME "rdaccel-${PROJECT_VERSION}-${RDA_OS}-${RDA_ARCH}")
  set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
  set(CPACK_SOURCE_GENERATOR TGZ)
  set(CPACK_SOURCE_PACKAGE_FILE_NAME "rdaccel-${PROJECT_VERSION}")
  set(CPACK_SOURCE_IGNORE_FILES "/[.]git/" "/build[^/]*/" "/inst[^/]*/" "/corpus/" "/crashes/")
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
    tests/test_motion.cpp
    tests/test_tile_cache.cpp
    tests/test_video_region.cpp
    tests/test_hevc_bitstream.cpp
    tests/test_lossless.cpp
    tests/test_local_pipe.cpp
    tests/test_tcp_transport.cpp
    tests/test_reliable_transport.cpp
    tests/test_session.cpp
    tests/test_cursor.cpp
    tests/test_link.cpp
    tests/test_keycodes.cpp
    tests/test_secure_transport.cpp
  )
  target_link_libraries(rdaccel_tests PRIVATE rdaccel_internal GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rdaccel_tests)

  # Platform codec round trip: encode synthetic frames with the platform
  # encoder and decode them with the platform decoder.
  # The public API tests use only include/rdaccel/: test_api.cpp the C++
  # wrapper (rdaccel.hpp), test_capi.cpp the C ABI (rdaccel.h).
  add_executable(rdaccel_platform_tests tests/test_platform_codec.cpp tests/test_api.cpp
    tests/test_capi.cpp)
  target_link_libraries(rdaccel_platform_tests PRIVATE rdaccel_internal GTest::gtest_main)
  if(NOT APPLE)
    target_sources(rdaccel_platform_tests PRIVATE tests/test_platform_x11.cpp)
    target_link_libraries(rdaccel_platform_tests PRIVATE PkgConfig::X11)
  endif()
  gtest_discover_tests(rdaccel_platform_tests DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")

  # The plain C example doubles as a test of the C ABI from C.
  if(RDACCEL_BUILD_EXAMPLES)
    add_test(NAME minimal_c COMMAND rdaccel_minimal_c)
    set_tests_properties(minimal_c PROPERTIES
      ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
  endif()
endif()

if(RDACCEL_BUILD_FUZZERS)
  foreach(name messages lossless viewer link secure reliable hevc)
    add_executable(fuzz_${name} fuzz/fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE rdaccel_internal)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(fuzz_${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz)
  endforeach()
endif()
