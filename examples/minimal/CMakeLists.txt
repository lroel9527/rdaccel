# A standalone project that uses an installed rdaccel, the way a third-party
# application would:
#
#   cmake --install build --prefix /some/prefix
#   cmake -S examples/minimal -B build-minimal -DCMAKE_PREFIX_PATH=/some/prefix
#   cmake --build build-minimal && ./build-minimal/minimal
cmake_minimum_required(VERSION 3.24)
project(rdaccel_minimal LANGUAGES CXX)

find_package(rdaccel 0.1 REQUIRED)

add_executable(minimal main.cpp)
target_link_libraries(minimal PRIVATE rdaccel::rdaccel)
