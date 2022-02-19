cmake_minimum_required(VERSION 3.15)

project(reaper_automidireset
  VERSION
    1.0.1
  DESCRIPTION
    "MIDI hardare plug-and-play for REAPER"
  LANGUAGES
    CXX
)

set(SOURCES ./reaper_automidireset.cpp)
set(LIBS "")
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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


add_library(automidireset SHARED ${SOURCES})
#set_target_properties(automidireset PROPERTIES VERSION ${PROJECT_VERSION})
target_include_directories(automidireset PRIVATE ../../WDL/WDL ../../sdk)
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
