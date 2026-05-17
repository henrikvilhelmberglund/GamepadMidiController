cmake_minimum_required(VERSION 3.14)
project(GamepadMidiController)

# Ensure C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# libremidi
FetchContent_Declare(
    libremidi
    GIT_REPOSITORY https://github.com/celtera/libremidi.git
    GIT_TAG        v5.3.1
)
# hidapi (direct Steam Controller HID access)
FetchContent_Declare(
    hidapi
    GIT_REPOSITORY https://github.com/libusb/hidapi.git
    GIT_TAG        hidapi-0.15.0
)

FetchContent_MakeAvailable(libremidi)

# Build hidapi as a static library so we don't have to ship hidapi.dll alongside the exe.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(hidapi)
unset(BUILD_SHARED_LIBS CACHE)

add_executable(GamepadMidiController
    GamepadMidiController.cpp
    GamepadMidiController.h
    mappings.h
    steam_controller.cpp
    steam_controller.h
)

target_link_libraries(GamepadMidiController PRIVATE libremidi::libremidi)
target_link_libraries(GamepadMidiController PRIVATE hidapi::hidapi)
