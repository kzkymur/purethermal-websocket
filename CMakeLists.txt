cmake_minimum_required(VERSION 3.20)
project(lepton_capture LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_LIBUVC "Enable PureThermal/libuvc capture support" ON)

add_executable(lepton_capture main.cpp)
find_package(Threads REQUIRED)
target_link_libraries(lepton_capture PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(lepton_capture PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(lepton_capture PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_compile_definitions(lepton_capture PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
  )
  target_link_libraries(lepton_capture PRIVATE
    mf mfplat mfreadwrite mfuuid ole32
  )
  message(STATUS "PureThermal capture enabled through Windows Media Foundation")
endif()

set(LIBUVC_FOUND FALSE)
if(ENABLE_LIBUVC AND NOT WIN32)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBUVC QUIET IMPORTED_TARGET libuvc)
    if(LIBUVC_FOUND)
      target_link_libraries(lepton_capture PRIVATE PkgConfig::LIBUVC)
    endif()
  endif()

  if(NOT LIBUVC_FOUND)
    find_path(LIBUVC_INCLUDE_DIR NAMES libuvc/libuvc.h)
    find_library(LIBUVC_LIBRARY NAMES uvc libuvc)
    find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
    if(LIBUVC_INCLUDE_DIR AND LIBUVC_LIBRARY)
      set(LIBUVC_FOUND TRUE)
      target_include_directories(lepton_capture PRIVATE "${LIBUVC_INCLUDE_DIR}")
      target_link_libraries(lepton_capture PRIVATE "${LIBUVC_LIBRARY}")
      if(LIBUSB_LIBRARY)
        target_link_libraries(lepton_capture PRIVATE "${LIBUSB_LIBRARY}")
      endif()
    endif()
  endif()
endif()

set(LEPTON_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/GetThermal/lepton_sdk")
if(LIBUVC_FOUND AND EXISTS "${LEPTON_SDK_ROOT}/Inc/LEPTON_SDK.h")
  message(STATUS "PureThermal capture enabled")
  target_compile_definitions(lepton_capture PRIVATE USE_LIBUVC=1)
  target_include_directories(lepton_capture PRIVATE "${LEPTON_SDK_ROOT}/Inc")
  target_sources(lepton_capture PRIVATE
    "${LEPTON_SDK_ROOT}/Src/LEPTON_SDK.c"
    "${LEPTON_SDK_ROOT}/Src/LEPTON_SYS.c"
  )
  if(MSVC)
    set_source_files_properties("${LEPTON_SDK_ROOT}/Src/LEPTON_SDK.c"
      PROPERTIES COMPILE_OPTIONS "/FIuvc_sdk.h")
  else()
    set_source_files_properties("${LEPTON_SDK_ROOT}/Src/LEPTON_SDK.c"
      PROPERTIES COMPILE_OPTIONS "-include;uvc_sdk.h")
  endif()
else()
  if(NOT WIN32)
    message(STATUS "PureThermal capture disabled; dummy mode remains available")
  endif()
  if(ENABLE_LIBUVC AND NOT LIBUVC_FOUND AND NOT WIN32)
    message(STATUS "Install libuvc or set LIBUVC_INCLUDE_DIR and LIBUVC_LIBRARY")
  elseif(LIBUVC_FOUND)
    message(STATUS "Initialize submodules to enable PureThermal capture")
  endif()
endif()
