# Example programs. They use only the public API (include/rdaccel/) and link
# rdaccel::rdaccel exactly as an application would.

add_executable(rdaccel_record record.cpp)
target_link_libraries(rdaccel_record PRIVATE rdaccel::rdaccel)

add_executable(rdaccel_play play.cpp)
target_link_libraries(rdaccel_play PRIVATE rdaccel::rdaccel)

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
endif()
