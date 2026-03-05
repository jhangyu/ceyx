cmake_minimum_required(VERSION 3.14)
project(dng_decoder_native VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(APPLE)
    add_compile_definitions(qMacOS=1 qWinOS=0)
elseif(WIN32)
    add_compile_definitions(qMacOS=0 qWinOS=1)
elseif(ANDROID)
    add_compile_definitions(qAndroid=1 qMacOS=0 qWinOS=0)
elseif(UNIX)
    add_compile_definitions(qLinux=1 qMacOS=0 qWinOS=0)
endif()

find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)

# Adobe DNG SDK
set(DNG_SDK_DIR ${THIRD_PARTY_DIR}/dng_sdk/source)
file(GLOB_RECURSE DNG_SOURCES "${DNG_SDK_DIR}/*.cpp")
list(REMOVE_ITEM DNG_SOURCES "${DNG_SDK_DIR}/dng_xmp_sdk.cpp" "${DNG_SDK_DIR}/dng_xmp.cpp")

# Inject our XMP stubs into the base DNG SDK library so its internal references resolve
list(APPEND DNG_SOURCES "${SRC_DIR}/dng_xmp_stub.cpp")

add_library(dng_sdk STATIC ${DNG_SOURCES})
target_include_directories(dng_sdk PUBLIC ${DNG_SDK_DIR})
target_compile_definitions(dng_sdk PUBLIC qDNGThreadSafe=1 qDNGXMPDocOps=0 qDNGXMPFiles=0 qDNGUseLibJPEG=1)
target_include_directories(dng_sdk PUBLIC ${JPEG_INCLUDE_DIRS})
target_link_libraries(dng_sdk ZLIB::ZLIB ${JPEG_LIBRARIES})

# Native library
file(GLOB_RECURSE NATIVE_SOURCES "${SRC_DIR}/*.cpp")
# Only add target if there are sources, dummy for now to avoid target creation error
if (NOT NATIVE_SOURCES)
    file(WRITE ${SRC_DIR}/dummy.cpp "void dummy(){}")
    set(NATIVE_SOURCES ${SRC_DIR}/dummy.cpp)
endif()

add_library(dng_decoder_native SHARED ${NATIVE_SOURCES})
target_include_directories(dng_decoder_native PUBLIC ${INC_DIR})
target_link_libraries(dng_decoder_native dng_sdk)

# Halide setup — link the full Halide library for JIT
set(HALIDE_DIR ${THIRD_PARTY_DIR}/halide)
list(APPEND CMAKE_PREFIX_PATH ${HALIDE_DIR})
find_package(Halide REQUIRED)
target_include_directories(dng_decoder_native PUBLIC ${HALIDE_DIR}/include)
target_link_libraries(dng_decoder_native Halide::Halide)

if(APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(CORESERVICES_LIBRARY CoreServices)
    target_link_libraries(dng_decoder_native ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})

    set_target_properties(dng_decoder_native PROPERTIES
        FRAMEWORK FALSE
        MACOSX_RPATH TRUE
    )
endif()

# Test application
add_executable(test_dng_decoder tests/test_main.cpp)
target_include_directories(test_dng_decoder PRIVATE ${INC_DIR})
target_link_libraries(test_dng_decoder dng_decoder_native Halide::Halide)

# Color accuracy / visual regression test
add_executable(test_color_accuracy tests/test_color_accuracy.cpp)
target_include_directories(test_color_accuracy PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_color_accuracy dng_sdk)
if(APPLE)
    target_link_libraries(test_color_accuracy ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()
