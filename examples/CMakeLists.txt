# Example programs. They use only the public API (include/rdaccel/) and link
# rdaccel::rdaccel exactly as an application would.

add_executable(rdaccel_record record.cpp)
target_link_libraries(rdaccel_record PRIVATE rdaccel::rdaccel)

add_executable(rdaccel_play play.cpp)
target_link_libraries(rdaccel_play PRIVATE rdaccel::rdaccel)

add_executable(rdaccel_bench bench.cpp)
target_link_libraries(rdaccel_bench PRIVATE rdaccel::rdaccel)

# macOS: host and viewer in one process, showing the decoded screen in a window.
if(APPLE)
  add_executable(rdaccel_loopback MACOSX_BUNDLE loopback_mac/main.mm)
  target_compile_options(rdaccel_loopback PRIVATE -fobjc-arc)
  target_link_libraries(rdaccel_loopback PRIVATE
    rdaccel::rdaccel
    "-framework AppKit"
    "-framework QuartzCore"
  )
  set_target_properties(rdaccel_loopback PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/loopback_mac/Info.plist.in)

  # A windowed viewer that controls another machine running rdaccel_serve.
  add_executable(rdaccel_remote remote_mac/main.mm)
  target_compile_options(rdaccel_remote PRIVATE -fobjc-arc)
  target_link_libraries(rdaccel_remote PRIVATE
    rdaccel::rdaccel
    "-framework AppKit"
    "-framework QuartzCore"
  )
endif()

add_executable(rdaccel_netsim netsim.cpp)
target_link_libraries(rdaccel_netsim PRIVATE rdaccel::rdaccel)

# Host and viewer on two machines, over UDP or TCP.
add_executable(rdaccel_serve serve.cpp)
target_link_libraries(rdaccel_serve PRIVATE rdaccel::rdaccel)
if(APPLE)
  target_link_libraries(rdaccel_serve PRIVATE "-framework CoreFoundation")
endif()
add_executable(rdaccel_view view.cpp)
target_link_libraries(rdaccel_view PRIVATE rdaccel::rdaccel)

# The same in plain C, against the C ABI alone.
add_executable(rdaccel_minimal_c minimal_c/main.c)
set_target_properties(rdaccel_minimal_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  LINKER_LANGUAGE CXX)
target_compile_options(rdaccel_minimal_c PRIVATE -pedantic)
target_link_libraries(rdaccel_minimal_c PRIVATE rdaccel::rdaccel)

if(RDACCEL_INSTALL AND RDACCEL_INSTALL_TOOLS)
  set(RDA_TOOLS rdaccel_serve rdaccel_view rdaccel_record rdaccel_play rdaccel_bench rdaccel_netsim)
  if(APPLE)
    list(APPEND RDA_TOOLS rdaccel_remote)
  endif()
  # Find a shared librdaccel next to them: <prefix>/bin -> <prefix>/lib.
  if(APPLE)
    set(RDA_TOOL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  else()
    set(RDA_TOOL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  endif()
  set_target_properties(${RDA_TOOLS} PROPERTIES INSTALL_RPATH "${RDA_TOOL_RPATH}")
  install(TARGETS ${RDA_TOOLS} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
