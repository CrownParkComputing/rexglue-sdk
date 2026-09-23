cmake_minimum_required(VERSION 3.25)
project(rayview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# raylib: built once from ~/raylib-src into build-linux.
set(RAYLIB_SRC "$ENV{HOME}/raylib-src" CACHE PATH "raylib source checkout")
add_library(raylib STATIC IMPORTED)
set_target_properties(raylib PROPERTIES
  IMPORTED_LOCATION "${RAYLIB_SRC}/build-linux/raylib/libraylib.a"
  INTERFACE_INCLUDE_DIRECTORIES "${RAYLIB_SRC}/src")

add_executable(rayview main.cpp)
target_link_libraries(rayview PRIVATE raylib m pthread dl rt GL X11 Xrandr Xinerama Xi Xcursor Xext)
