cmake_minimum_required(VERSION 3.16)

project(ocpp LANGUAGES C CXX)

include(${CMAKE_CURRENT_LIST_DIR}/build.cmake)

add_library(${PROJECT_NAME} STATIC ${OCPP_SRCS})

target_compile_features(${PROJECT_NAME} PRIVATE c_std_99)

target_include_directories(${PROJECT_NAME}
	PUBLIC ${OCPP_INCS}
)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
# TODO: build for tests
endif()

