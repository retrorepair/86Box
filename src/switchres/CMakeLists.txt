# switchres, vendored as a pure modeline calculator.
#
# SR_CALC_ONLY drops the platform display backends (display_linux/windows/sdl2 and their
# custom_video_* dependencies on X11/DRM/ADL/ATI), leaving only the "dummy" path: no host
# display is opened and timings are computed from the monitor preset's frequency ranges.
# That is all this integration wants - the MiSTer does the actual display switching.

add_library(switchres OBJECT
    switchres.cpp
    monitor.cpp
    modeline.cpp
    display.cpp
    custom_video.cpp
    log.cpp
    edid.cpp
    switchres_wrapper.cpp
)

target_compile_definitions(switchres PUBLIC SR_CALC_ONLY SR_WIN32_STATIC)

target_include_directories(switchres PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT MSVC)
    target_compile_options(switchres PRIVATE -w)
endif()
