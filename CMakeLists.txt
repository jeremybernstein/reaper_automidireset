cmake_minimum_required(VERSION 3.15)

project(reaper_automidireset
  VERSION
    1.4.0
  DESCRIPTION
    "MIDI hardare plug-and-play for REAPER"
  LANGUAGES
    CXX
)

set(SOURCES ./reaper_automidireset.cpp)
set(LIBS "")
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

if (APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architecture" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.11" CACHE STRING "Minimum OS X deployment version" FORCE)
    find_library(CF_FRAMEWORK CoreFoundation)
    find_library(CM_FRAMEWORK CoreMIDI)
    set(LIBS ${CF_FRAMEWORK} ${CM_FRAMEWORK})
endif ()

if (WIN32)
    add_definitions(-DUNICODE -D_UNICODE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(LIBS user32.lib)
endif ()

if (LINUX)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    find_path(LIBUSB_INCLUDE_DIR 
        NAMES libusb.h 
        PATH_SUFFIXES "include" "libusb" "libusb-1.0")
    find_library(LIBUSB_LIBRARY
        NAMES usb-1.0)
    add_definitions(-DNOMINMAX)
    set(INCLUDES ${LIBUSB_INCLUDE_DIR})
    set(LIBS ${LIBUSB_LIBRARY})
endif ()

add_library(automidireset SHARED ${SOURCES})
target_include_directories(automidireset PRIVATE ${INCLUDES} ../../WDL/WDL ../../sdk)
target_link_libraries(automidireset ${LIBS})
set_target_properties(automidireset PROPERTIES PREFIX "")
set_target_properties(automidireset PROPERTIES OUTPUT_NAME "reaper_automidireset")

if (APPLE)
    set_target_properties(automidireset PROPERTIES SUFFIX ".dylib")
endif ()

if (WIN32)
    if ("${CMAKE_GENERATOR_PLATFORM}" MATCHES "Win32")
        set_target_properties(automidireset PROPERTIES SUFFIX "_w32.dll")
    else ()
        set_target_properties(automidireset PROPERTIES SUFFIX "_x64.dll")
    endif ()
endif ()
