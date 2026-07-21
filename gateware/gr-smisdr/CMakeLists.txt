cmake_minimum_required(VERSION 3.16)
project(gr-smisdr CXX C)

# ---------------------------------------------------------------------------
# smiSDR In-Band-Signaling Encoder/Decoder — GNU Radio 3.10 OOT-Modul
#
# Baut zwei Blöcke:
#   gr::smisdr::encoder  complex -> short   (16-Bit smiBus/PARLIO Protokoll)
#   gr::smisdr::decoder  short   -> complex + Message-Port "cmd"
# ---------------------------------------------------------------------------

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

find_package(Gnuradio "3.10" REQUIRED COMPONENTS runtime pmt)

find_package(Python3 COMPONENTS Interpreter Development)
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
    # Fallback: nutzt pybind11, das mit gnuradio-dev / python3-pybind11 kommt
    find_package(PythonLibs REQUIRED)
    include(FindPkgConfig)
    pkg_check_modules(PYBIND11 REQUIRED pybind11)
endif()

# Versionsinfo
set(SMISDR_VERSION_MAJOR 1)
set(SMISDR_VERSION_MINOR 0)
set(SMISDR_VERSION_PATCH 0)

include(GNUInstallDirs)

install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

add_subdirectory(include/gnuradio/smisdr)
add_subdirectory(lib)
add_subdirectory(python/smisdr)
add_subdirectory(grc)

enable_testing()
