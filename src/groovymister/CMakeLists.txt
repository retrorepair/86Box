# GroovyMiSTer client, vendored from https://github.com/psakhis/Groovy_MiSTer (api/).
# Built in rather than loaded from a DLL, so there is no runtime dlopen surface.

add_library(groovymister OBJECT
    groovymister.cpp
    groovymister_wrapper.cpp
    nlc_codec.cpp
    lz4/lz4.c
    lz4/lz4hc.c
)

target_compile_definitions(groovymister PUBLIC GROOVYMISTER_WIN32_STATIC)

target_include_directories(groovymister PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    # RIO (registered I/O) for the low-latency UDP sender, plus standard winsock.
    target_link_libraries(groovymister PUBLIC ws2_32 mswsock)
endif()

# Vendored third-party code is built warning-free.
if(NOT MSVC)
    target_compile_options(groovymister PRIVATE -w)
endif()
