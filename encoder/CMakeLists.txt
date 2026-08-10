cmake_minimum_required(VERSION 3.16)
project(iso21496_encoder VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

add_library(iso21496_core STATIC
  src/common.cpp
  src/color.cpp
  src/icc.cpp
  src/tiff_reader.cpp
  src/pipeline.cpp
  src/jpeg_encoder.cpp
  src/iso_metadata.cpp
  src/exif.cpp
  src/encoder.cpp
  src/options.cpp
)
target_include_directories(iso21496_core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR})

find_package(Threads REQUIRED)
target_link_libraries(iso21496_core PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(iso21496_core PRIVATE /W4 /O2 /fp:fast)
  target_compile_definitions(iso21496_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(iso21496_core PRIVATE
    -Wall -Wextra -Wno-unused-parameter -ffp-contract=fast)
endif()

# Static-link the toolchain runtimes so the binary the plugin ships has no
# external dependencies on the photographer's machine. Set globally rather than
# per target: anything linking iso21496_core has to agree with it, and the test
# executables silently did not.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
      CACHE STRING "" FORCE)
  set_property(TARGET iso21496_core PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(iso21496_encoder src/main.cpp)
target_link_libraries(iso21496_encoder PRIVATE iso21496_core)
if(MSVC)
  set_property(TARGET iso21496_encoder PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(NOT APPLE)
  target_link_options(iso21496_encoder PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

# The HEIC repackager. macOS only, and deliberately a second program: it needs
# an HEVC encoder, which means ImageIO, and the whole point of the encoder above
# is that it has no dependencies on any platform.
#
# Built but not shipped, and parked rather than pending — it is kept as the
# fallback if Apple ever breaks the JPEG path again. See CLAUDE.md for why it
# is not an export option.
if(APPLE)
  enable_language(OBJC)
  add_executable(iso21496_heic src/heic_tool.m)
  target_compile_options(iso21496_heic PRIVATE -fobjc-arc)
  target_link_libraries(iso21496_heic PRIVATE
    "-framework Foundation" "-framework CoreGraphics" "-framework ImageIO")
  install(TARGETS iso21496_heic RUNTIME DESTINATION bin)
endif()

option(ISO21496_BUILD_TESTS "Build the test suite" ON)
if(ISO21496_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS iso21496_encoder RUNTIME DESTINATION bin)
