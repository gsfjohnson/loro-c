# Standalone consumer project: proves an *installed* loro can be found via find_package and
# linked through the public loro::loro target. Not part of the main build — point it at an
# install tree:
#   cmake -S tests/consumer -B build-consumer -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build build-consumer
#   ctest --test-dir build-consumer --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(loro_consumer LANGUAGES CXX)

find_package(loro REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE loro::loro)

enable_testing()
add_test(NAME consumer COMMAND consumer)
