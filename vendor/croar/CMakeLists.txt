cmake_minimum_required(VERSION 3.10)
project(croar)

add_library(roaring STATIC roaring.c)
add_library(roaring::roaring ALIAS roaring)

target_include_directories(roaring PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(roaring  PROPERTIES FOLDER "vendor/croaring")
 