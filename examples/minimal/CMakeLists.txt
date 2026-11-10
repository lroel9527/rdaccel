# A standalone project that uses an installed rdaccel, the way a third-party
# application would:
#
#   cmake --install build --prefix /some/prefix
#   cmake -S examples/minimal -B build-minimal -DCMAKE_PREFIX_PATH=/some/prefix
#   cmake --build build-minimal && ./build-minimal/minimal
cmake_minimum_required(VERSION 3.24)
project(rdaccel_minimal LANGUAGES C CXX)

find_package(rdaccel 1.4 REQUIRED)

add_executable(minimal main.cpp)
target_link_libraries(minimal PRIVATE rdaccel::rdaccel)

# The plain C version, against the C ABI alone (linked by the C++ linker
# because the static library needs the C++ runtime).
add_executable(minimal_c ../minimal_c/main.c)
set_target_properties(minimal_c PROPERTIES C_STANDARD 99 LINKER_LANGUAGE CXX)
target_link_libraries(minimal_c PRIVATE rdaccel::rdaccel)
