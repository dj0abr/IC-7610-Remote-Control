cmake_minimum_required(VERSION 3.20)
project(kmcontrol LANGUAGES CXX Fortran)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW REQUIRED IMPORTED_TARGET fftw3f)

set(LIQUID_ROOT "/Daten/funk/sat/QO100_Transceiver/libs" CACHE PATH "LiquidDSP installation root")
find_path(LIQUID_INCLUDE_DIR liquid.h HINTS "${LIQUID_ROOT}/include" PATH_SUFFIXES liquid)
find_library(LIQUID_LIBRARY NAMES liquid
    HINTS "${LIQUID_ROOT}/${CMAKE_SYSTEM_PROCESSOR}" "${LIQUID_ROOT}/lib")
if(NOT LIQUID_INCLUDE_DIR OR NOT LIQUID_LIBRARY)
    message(FATAL_ERROR "LiquidDSP nicht gefunden. LIQUID_ROOT oder LIQUID_INCLUDE_DIR und LIQUID_LIBRARY setzen.")
endif()

add_library(LiquidDSP UNKNOWN IMPORTED)
set_target_properties(LiquidDSP PROPERTIES
    IMPORTED_LOCATION "${LIQUID_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIQUID_INCLUDE_DIR}")


set(SPEEXDSP_ROOT "/Daten/funk/sat/QO100_TRX2/external/speexdsp-1.2.1" CACHE PATH "SpeexDSP bundle root")
set(SPEEXDSP_ARCHIVE "${SPEEXDSP_ROOT}/lib/linux-${CMAKE_SYSTEM_PROCESSOR}/libspeexdsp.a")
if(EXISTS "${SPEEXDSP_ARCHIVE}" AND EXISTS "${SPEEXDSP_ROOT}/include/speex/speex_preprocess.h")
    add_library(SpeexDSP STATIC IMPORTED)
    set_target_properties(SpeexDSP PROPERTIES IMPORTED_LOCATION "${SPEEXDSP_ARCHIVE}"
        INTERFACE_INCLUDE_DIRECTORIES "${SPEEXDSP_ROOT}/include" INTERFACE_LINK_LIBRARIES m)
    set(SPEEXDSP_TARGET SpeexDSP)
else()
    pkg_check_modules(SPEEXDSP REQUIRED IMPORTED_TARGET speexdsp)
    set(SPEEXDSP_TARGET PkgConfig::SPEEXDSP)
endif()

add_library(icom-lan STATIC icom-lan.cpp)
target_compile_options(icom-lan PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kmcontrol-backend backend.cpp main-control.cpp spectrum.cpp ssb-demod.cpp
    rx-audio-filter.cpp rx-notch.cpp ft-receive.cpp ft-transmit.cpp ft_logbook.cpp)
target_link_libraries(kmcontrol-backend PRIVATE Qt5::Core Qt5::Network PkgConfig::FFTW LiquidDSP ${SPEEXDSP_TARGET} icom-lan)
target_compile_options(kmcontrol-backend PRIVATE -Wall -Wextra -Wpedantic)

find_package(Boost 1.74 REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(external/ft8)
set_target_properties(ft8-decoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(kmcontrol-backend ft8-decoder)
target_link_libraries(kmcontrol-backend PRIVATE ft8_core)
target_include_directories(kmcontrol-backend PRIVATE external/ft8/src)

add_executable(kmcontrol-web web-bridge.cpp web.qrc)
target_link_libraries(kmcontrol-web PRIVATE Qt5::Core Boost::boost Threads::Threads)
target_compile_definitions(kmcontrol-web PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
target_compile_options(kmcontrol-web PRIVATE -Wall -Wextra -Wpedantic)
