cmake_minimum_required(VERSION 3.16)
project(stackchan_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Add one host test executable and register it with ctest.
#   anan_add_host_test(<name> <sources...>)
# Example (later tasks):
#   anan_add_host_test(test_head_gestures test_head_gestures.cc ../head_gestures.cc)
# Warnings are errors, and asserts are always active (NDEBUG is undefined even
# in Release builds) because the tests rely on <cassert>.
function(anan_add_host_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

anan_add_host_test(test_behavior_core test_behavior_core.cc ../behavior_core.cc)
