cmake_minimum_required(VERSION 3.24)
project(KanekistReplay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WEBVIEW2_ROOT "${CMAKE_SOURCE_DIR}/third_party/webview2/build/native")

add_library(kanekist_core
    src/core.cpp
    src/replay_buffer.cpp
    src/media_engine.cpp
    src/wgc_capture.cpp
    src/game_window.cpp
    src/audio_capture.cpp
    src/audio_mixer.cpp
    src/fps_monitor.cpp
    src/overlay_menu.cpp
    src/replay_recorder.cpp
)
target_include_directories(kanekist_core PUBLIC src)
target_compile_definitions(kanekist_core PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_compile_options(kanekist_core PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(kanekist_core PUBLIC
    d3d11 dxgi dxguid
    mf mfplat mfreadwrite mfuuid
    ole32 propsys avrt winmm windowscodecs
    dwmapi shlwapi comctl32 uxtheme d2d1 dwrite shell32
    runtimeobject windowsapp
)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(UI_EMBED_HEADER "${CMAKE_BINARY_DIR}/generated/ui_embed.h")
add_custom_command(
    OUTPUT "${UI_EMBED_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_ui.py"
        "${CMAKE_SOURCE_DIR}/ui/index.html" "${UI_EMBED_HEADER}"
    DEPENDS "${CMAKE_SOURCE_DIR}/ui/index.html" "${CMAKE_SOURCE_DIR}/tools/embed_ui.py"
    VERBATIM
)
add_custom_target(kanekist_embed_ui DEPENDS "${UI_EMBED_HEADER}")

add_executable(KanekistReplay WIN32
    src/main.cpp
    src/app.cpp
    src/app.manifest
)
target_include_directories(KanekistReplay PRIVATE
    "${WEBVIEW2_ROOT}/include"
    "${CMAKE_BINARY_DIR}/generated"
)
target_link_libraries(KanekistReplay PRIVATE
    kanekist_core
    "${WEBVIEW2_ROOT}/x64/WebView2LoaderStatic.lib"
    version
)
target_compile_options(KanekistReplay PRIVATE /W4 /permissive- /EHsc /utf-8)
add_dependencies(KanekistReplay kanekist_embed_ui)
set_source_files_properties(src/app.cpp PROPERTIES OBJECT_DEPENDS "${UI_EMBED_HEADER}")
set_target_properties(KanekistReplay PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:KanekistReplay>"
)
add_custom_command(TARGET KanekistReplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tools/PresentMon.exe"
        "$<TARGET_FILE_DIR:KanekistReplay>/PresentMon.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/tools/ffmpeg-runtime"
        "$<TARGET_FILE_DIR:KanekistReplay>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:KanekistReplay>/ui"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(kanekist_tests tests/tests.cpp)
    target_link_libraries(kanekist_tests PRIVATE kanekist_core)
    target_compile_options(kanekist_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    add_test(NAME kanekist_tests COMMAND kanekist_tests)
endif()

install(TARGETS KanekistReplay RUNTIME DESTINATION .)
