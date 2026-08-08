set(ISO21496_TEST_TMPDIR ${CMAKE_CURRENT_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${ISO21496_TEST_TMPDIR})

add_library(test_support STATIC test_support.cpp)
target_link_libraries(test_support PUBLIC iso21496_core)
target_include_directories(test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_support PUBLIC
  ISO21496_TEST_TMPDIR="${ISO21496_TEST_TMPDIR}")

foreach(name tiff color jpeg metadata shaping endtoend)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE test_support)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# Optional: decode our own output with libjpeg to prove the bitstream is real.
#
# Two macOS traps. Mono installs a jpeglib.h into /Library/Frameworks whose
# jpeg_mem_src predates the const-correct signature, and find_path picks it up
# ahead of the library CMake actually links — which is what has kept the macOS
# CI job red. And Homebrew ships a single-architecture libjpeg, so a universal
# build cannot link this test at all; the encoder itself has no such dependency,
# which is the whole point of it being a build-time check.
if(APPLE)
  list(APPEND CMAKE_IGNORE_PATH /Library/Frameworks/Mono.framework/Headers)
endif()

list(LENGTH CMAKE_OSX_ARCHITECTURES ISO21496_ARCH_COUNT)
if(ISO21496_ARCH_COUNT GREATER 1)
  message(STATUS "universal build: skipping the decode round-trip test "
                 "(libjpeg is single-architecture). Build natively to run it.")
  set(JPEG_FOUND FALSE)
else()
  find_package(JPEG QUIET)
endif()

if(JPEG_FOUND)
  add_executable(test_decode test_decode.cpp)
  target_link_libraries(test_decode PRIVATE test_support JPEG::JPEG)
  add_test(NAME decode COMMAND test_decode)
  message(STATUS "libjpeg found: enabling the decode round-trip test")
else()
  message(STATUS "libjpeg not found: skipping the decode round-trip test")
endif()

# Optional: exiftool validates the acceptance criteria from the build spec.
find_program(EXIFTOOL_EXECUTABLE exiftool)
if(EXIFTOOL_EXECUTABLE)
  add_test(NAME exiftool_compliance
           COMMAND ${CMAKE_COMMAND}
                   -DENCODER=$<TARGET_FILE:iso21496_encoder>
                   -DEXIFTOOL=${EXIFTOOL_EXECUTABLE}
                   -DTMPDIR=${ISO21496_TEST_TMPDIR}
                   -DFIXTURE=$<TARGET_FILE:make_fixture>
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/exiftool_check.cmake)
  message(STATUS "exiftool found: enabling the metadata compliance test")
else()
  message(STATUS "exiftool not found: skipping the metadata compliance test")
endif()

add_executable(make_fixture make_fixture.cpp)
target_link_libraries(make_fixture PRIVATE test_support)
