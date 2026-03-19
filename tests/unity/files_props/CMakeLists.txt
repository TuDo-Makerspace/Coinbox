cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Keep build scope tight to speed up local test iteration.
idf_build_set_property(MINIMAL_BUILD ON)
project(coinbox_files_props_test)
