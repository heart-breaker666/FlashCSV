cmake_minimum_required(VERSION 3.16)
project(FlashCSV VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

add_library(flash_csv_core src/csv.cpp src/query.cpp src/aggregate.cpp src/pipeline.cpp src/engine.cpp)
target_include_directories(flash_csv_core PUBLIC include)
target_compile_options(flash_csv_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_link_libraries(flash_csv_core PUBLIC Threads::Threads)

add_executable(flash_csv src/main.cpp)
target_link_libraries(flash_csv PRIVATE flash_csv_core)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE flash_csv_core)
  add_test(NAME core COMMAND core_tests)
  set_tests_properties(core PROPERTIES TIMEOUT 20)
  add_executable(engine_tests tests/engine_tests.cpp)
  target_link_libraries(engine_tests PRIVATE flash_csv_core)
  add_test(NAME engine COMMAND engine_tests ${CMAKE_CURRENT_BINARY_DIR}/test-output)
  set_tests_properties(engine PROPERTIES TIMEOUT 20)
  add_test(NAME acceptance COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance.py $<TARGET_FILE:flash_csv>
    ${CMAKE_CURRENT_BINARY_DIR}/test-output)
  set_tests_properties(acceptance PROPERTIES TIMEOUT 90)
endif()
