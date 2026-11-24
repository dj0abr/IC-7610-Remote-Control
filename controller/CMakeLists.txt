cmake_minimum_required(VERSION 3.16)
project(kmcontrol LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW REQUIRED IMPORTED_TARGET fftw3f)

set(LIQUID_ROOT "" CACHE PATH "Optional LiquidDSP installation root")
find_path(LIQUID_INCLUDE_DIR liquid.h HINTS "${LIQUID_ROOT}" PATH_SUFFIXES include include/liquid liquid)
find_library(LIQUID_LIBRARY NAMES liquid
    HINTS "${LIQUID_ROOT}" PATH_SUFFIXES "${CMAKE_SYSTEM_PROCESSOR}" lib lib64)
if(NOT LIQUID_INCLUDE_DIR OR NOT LIQUID_LIBRARY)
    message(FATAL_ERROR "LiquidDSP nicht gefunden. LIQUID_ROOT oder LIQUID_INCLUDE_DIR und LIQUID_LIBRARY setzen.")
endif()

add_library(LiquidDSP UNKNOWN IMPORTED)
set_target_properties(LiquidDSP PROPERTIES
    IMPORTED_LOCATION "${LIQUID_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIQUID_INCLUDE_DIR}")


add_library(icom-lan STATIC icom-lan.cpp)
target_compile_options(icom-lan PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kmcontrol-backend backend.cpp main-control.cpp spectrum.cpp ssb-demod.cpp)
target_link_libraries(kmcontrol-backend PRIVATE Qt5::Core Qt5::Network PkgConfig::FFTW LiquidDSP icom-lan)
target_compile_options(kmcontrol-backend PRIVATE -Wall -Wextra -Wpedantic)

find_package(Boost 1.74 REQUIRED)
find_package(Threads REQUIRED)

add_executable(kmcontrol-web web-bridge.cpp web.qrc)
target_link_libraries(kmcontrol-web PRIVATE Qt5::Core Boost::boost Threads::Threads)
target_compile_definitions(kmcontrol-web PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
target_compile_options(kmcontrol-web PRIVATE -Wall -Wextra -Wpedantic)
