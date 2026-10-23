cmake_minimum_required(VERSION 3.20)
project(wintab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xext xdamage xcomposite)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

set(WINTAB_CORE
    src/config.cpp
    src/toml_lite.cpp
    src/touchpad.cpp
    src/gesture.cpp
    src/x11.cpp
    src/windows.cpp
    src/image.cpp
    src/cards.cpp
    src/capture.cpp
    src/overlay.cpp
    src/pointer.cpp
    src/app.cpp
)

add_library(wintab_core STATIC ${WINTAB_CORE})
target_include_directories(wintab_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(wintab_core
    PUBLIC PkgConfig::GTK3 PkgConfig::CAIRO PkgConfig::X11 PkgConfig::GIO)
target_compile_options(wintab_core PRIVATE -Wall -Wextra)

add_executable(wintab src/main.cpp)
target_link_libraries(wintab PRIVATE wintab_core)
target_compile_options(wintab PRIVATE -Wall -Wextra)

# --- tools ----------------------------------------------------------------
add_executable(probe_windows tools/probe_windows.cpp)
target_link_libraries(probe_windows PRIVATE wintab_core)
target_include_directories(probe_windows PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Replay a recording through the gesture engine and print the events, for
# diffing against the Python build.
add_executable(trace_replay tools/trace_replay.cpp)
target_link_libraries(trace_replay PRIVATE wintab_core)
target_include_directories(trace_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Drives a synthetic swipe through the real stack and checks the pixels.
add_executable(spike_carousel tools/spike_carousel.cpp)
target_link_libraries(spike_carousel PRIVATE wintab_core)
target_include_directories(spike_carousel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Proves the overlay window recipe under the running compositor.
add_executable(spike_overlay tools/spike_overlay.cpp)
target_link_libraries(spike_overlay PRIVATE wintab_core)
target_include_directories(spike_overlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- tests ----------------------------------------------------------------
enable_testing()
add_executable(wintab_tests
    tests/support/harness.cpp
    tests/test_config.cpp
    tests/test_gesture.cpp
    tests/test_windows.cpp
    tests/test_overlay.cpp
    tests/test_touchpad.cpp
    tests/test_capture.cpp
)
target_include_directories(wintab_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(wintab_tests PRIVATE wintab_core)
target_compile_options(wintab_tests PRIVATE -Wall -Wextra)
add_test(NAME wintab_tests COMMAND wintab_tests)

install(TARGETS wintab RUNTIME DESTINATION bin)
