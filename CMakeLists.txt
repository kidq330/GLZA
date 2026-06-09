cmake_minimum_required(VERSION 3.25)
project(glza LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GLZA_ASAN "Enable AddressSanitizer" OFF)
option(GLZA_PRINTON "Enable debug fprintf output" ON)

set(GLZA_SOURCES
    glza_model.cpp
    glza_format.cpp
    glza_compress.cpp
    glza_encode.cpp
    glza_decode.cpp
    glza_pipeline.cpp
    glza_api.cpp
)

add_library(glza_lib STATIC ${GLZA_SOURCES})
target_include_directories(glza_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(glza_lib PRIVATE m)

if(GLZA_PRINTON)
  target_compile_definitions(glza_lib PUBLIC PRINTON)
endif()

if(GLZA_ASAN)
  target_compile_options(glza_lib PUBLIC -fsanitize=address -fno-omit-frame-pointer -g -O1)
  target_link_options(glza_lib PUBLIC -fsanitize=address)
else()
  target_compile_options(glza_lib PRIVATE -O3)
endif()

target_compile_options(glza_lib PRIVATE
    -Wall -Wextra -Wpedantic
    -D_FILE_OFFSET_BITS=64
)

add_executable(glza glza_cli.cpp)
target_link_libraries(glza PRIVATE glza_lib)

macro(add_test_harness name source)
  add_executable(${name} scripts/test/${source})
  set_source_files_properties(scripts/test/${source} PROPERTIES LANGUAGE CXX)
  target_compile_options(${name} PRIVATE -Wno-old-style-cast -Wno-zero-as-null-pointer-constant)
  target_link_libraries(${name} PRIVATE glza_lib)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endmacro()

add_test_harness(rt_large rt_large.c)
add_test_harness(rt_test rt_test.c)
add_test_harness(rt_save rt_save.c)
add_test_harness(rt_slow rt_slow.c)
add_test_harness(rt_fast0 rt_fast0.c)
add_test_harness(rt_author rt_author.c)
add_test_harness(rt_author10m rt_author10m.c)
add_test_harness(rt_same6500 rt_same6500.c)
add_test_harness(dec_only6500 dec_only6500.c)
add_test_harness(dec_only_mr dec_only_mr.c)

option(GLZA_FUZZ "Build libFuzzer harness" OFF)
if(GLZA_FUZZ)
  # Compile the GLZA sources directly into the fuzz target (rather than linking
  # the shared glza_lib) so the whole library is coverage-instrumented for
  # libFuzzer. Linking glza_lib instead would leave the library invisible to
  # the coverage feedback (black-box fuzzing), and instrumenting glza_lib in
  # place would break the glza CLI / rt_* harnesses that link it without the
  # sancov runtime.
  add_executable(fuzz_roundtrip fuzz_roundtrip.cpp ${GLZA_SOURCES})
  target_include_directories(fuzz_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(fuzz_roundtrip PRIVATE m)
  target_compile_definitions(fuzz_roundtrip PRIVATE $<$<BOOL:${GLZA_PRINTON}>:PRINTON>)
  target_compile_options(fuzz_roundtrip PRIVATE
      -fsanitize=fuzzer,address -fno-omit-frame-pointer -g -O1
      -D_FILE_OFFSET_BITS=64)
  target_link_options(fuzz_roundtrip PRIVATE -fsanitize=fuzzer,address)
endif()

enable_testing()

add_test(NAME quick_5000_1m COMMAND rt_large 5000 ${CMAKE_CURRENT_SOURCE_DIR}/enwik1m)
add_test(NAME quick_6000_1m COMMAND rt_large 6000 ${CMAKE_CURRENT_SOURCE_DIR}/enwik1m)
add_test(NAME quick_6325_1m COMMAND rt_large 6325 ${CMAKE_CURRENT_SOURCE_DIR}/enwik1m)
add_test(NAME quick_5000_10m COMMAND rt_large 5000 ${CMAKE_CURRENT_SOURCE_DIR}/enwik10m)
add_test(NAME quick_6000_10m COMMAND rt_large 6000 ${CMAKE_CURRENT_SOURCE_DIR}/enwik10m)
add_test(NAME quick_6325_10m COMMAND rt_large 6325 ${CMAKE_CURRENT_SOURCE_DIR}/enwik10m)
