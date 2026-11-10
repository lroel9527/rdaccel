# A remote-support host and viewer in plain C, built the way an outside
# project would build them: against an installed rdaccel, found with
# find_package (CMake) or pkg-config (see Makefile).
#
#   cmake --install build --prefix /some/prefix
#   cmake -S examples/integration -B build-integration -DCMAKE_PREFIX_PATH=/some/prefix
#   cmake --build build-integration
cmake_minimum_required(VERSION 3.24)
project(rdaccel_integration LANGUAGES C CXX)

find_package(rdaccel 1.6 REQUIRED)
find_package(Threads REQUIRED)

foreach(prog support_host support_viewer)
  add_executable(${prog} ${prog}.c)
  # C, but linked by the C++ linker: a static rdaccel needs the C++ runtime.
  set_target_properties(${prog} PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
  target_compile_definitions(${prog} PRIVATE _POSIX_C_SOURCE=200809L)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${prog} PRIVATE -Wall -Wextra -Werror)
  endif()
  target_link_libraries(${prog} PRIVATE rdaccel::rdaccel Threads::Threads)
endforeach()
