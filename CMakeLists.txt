cmake_minimum_required(VERSION 3.16)
project(CompressorStudio LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)

# ── ImGui (lives in libs/imgui) ──────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/libs/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

# ── All source files ─────────────────────────────────────────────────────────
add_executable(CompressorStudio
    main.cpp
    # Text codecs
    huffman.cpp
    codec.cpp
    rle.cpp
    lzw.cpp
    arith.cpp
    brotli.cpp
    # Image codecs
    quadtree.cpp
    image_codec.cpp
    jpeg_codec.cpp
    j2k_codec.cpp
    webp_codec.cpp
    png_codec.cpp
    flif_codec.cpp
    # ImGui
    ${IMGUI_SOURCES}
)

target_include_directories(CompressorStudio PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(CompressorStudio PRIVATE ${SDL2_LIBRARIES})

if(APPLE)
    find_library(COCOA_LIB    Cocoa)
    find_library(IOKIT_LIB    IOKit)
    find_library(COREVIDEO_LIB CoreVideo)
    target_link_libraries(CompressorStudio PRIVATE
        ${COCOA_LIB} ${IOKIT_LIB} ${COREVIDEO_LIB})
    set_target_properties(CompressorStudio PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.compressor.studio"
        MACOSX_BUNDLE_BUNDLE_NAME    "CompressorStudio"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0")
endif()

if(WIN32)
    set_target_properties(CompressorStudio PROPERTIES WIN32_EXECUTABLE TRUE)

endif()

if(MSVC)
    target_compile_options(CompressorStudio PRIVATE /W3 /O2)
    target_compile_definitions(CompressorStudio PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(CompressorStudio PRIVATE -Wall -O2)
endif()
