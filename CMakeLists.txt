cmake_minimum_required(VERSION 3.20)
project(lepton_ws_server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Homebrew installs a single Boost package without per-component CMake configs.
# Force CMake to use the classic FindBoost module instead of BoostConfig.cmake.
set(Boost_NO_BOOST_CMAKE ON)
# Only require Boost headers; Boost.System is header-only in modern Boost.
find_package(Boost REQUIRED)

add_executable(lepton_ws_server main.cpp)
target_include_directories(lepton_ws_server PRIVATE ${Boost_INCLUDE_DIRS})
# Prefer the interface headers target if provided by FindBoost
if(TARGET Boost::headers)
  target_link_libraries(lepton_ws_server PRIVATE Boost::headers)
endif()
# Ensure Boost.System is treated as header-only so no lib lookup is needed
target_compile_definitions(lepton_ws_server PRIVATE BOOST_ERROR_CODE_HEADER_ONLY=1)

target_compile_definitions(lepton_ws_server PRIVATE USE_LIBUVC=1)
# Try pkg-config first (Homebrew installs .pc files)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUVC QUIET libuvc)
endif()

if(LIBUVC_FOUND)
  target_include_directories(lepton_ws_server PRIVATE ${LIBUVC_INCLUDE_DIRS})
  target_link_directories(lepton_ws_server PRIVATE ${LIBUVC_LIBRARY_DIRS})
  target_link_libraries(lepton_ws_server PRIVATE ${LIBUVC_LIBRARIES})
else()
  # Fallback: search common Homebrew locations
  find_path(LIBUVC_INCLUDE_DIR libuvc/libuvc.h HINTS /opt/homebrew/include /usr/local/include)
  find_library(LIBUVC_LIB uvc HINTS /opt/homebrew/lib /usr/local/lib)
  find_library(LIBUSB_LIB usb-1.0 HINTS /opt/homebrew/lib /usr/local/lib)
  if(NOT LIBUVC_INCLUDE_DIR OR NOT LIBUVC_LIB)
    message(FATAL_ERROR "libuvc not found. Install via 'brew install libuvc' or set include/lib paths.")
  endif()
  target_include_directories(lepton_ws_server PRIVATE ${LIBUVC_INCLUDE_DIR})
  if(LIBUSB_LIB)
    target_link_libraries(lepton_ws_server PRIVATE ${LIBUVC_LIB} ${LIBUSB_LIB})
  else()
    target_link_libraries(lepton_ws_server PRIVATE ${LIBUVC_LIB})
  endif()
endif()
