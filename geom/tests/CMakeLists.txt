cmake_minimum_required(VERSION 3.20)

##### geom tests #####

add_executable(geom_tests geom_tests.cpp)

target_link_libraries(geom_tests geom)
