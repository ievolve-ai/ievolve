cmake_minimum_required(VERSION 3.24)
project(ievolve VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(CTest)

option(IEVOLVE_SUPERBUILD "Build pinned third-party dependencies first" ON)
option(IEVOLVE_ENABLE_SANITIZERS "Enable address and undefined behavior checks" OFF)

set(IEVOLVE_COMPONENTS utils config program prompt process llm code database evaluator controller cli)

if(IEVOLVE_SUPERBUILD)
  include(cmake/Dependencies.cmake)

  ExternalProject_Add(ievolve_project
    LIST_SEPARATOR "|"
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/ievolve"
    DOWNLOAD_COMMAND ""
    CMAKE_ARGS ${IEVOLVE_COMMON_CMAKE_ARGS}
      -DIEVOLVE_SUPERBUILD=OFF
      -DBUILD_TESTING=${BUILD_TESTING}
      -DIEVOLVE_ENABLE_SANITIZERS=${IEVOLVE_ENABLE_SANITIZERS}
      -DCMAKE_PREFIX_PATH=${IEVOLVE_DEPENDENCY_PREFIX}
    DEPENDS ${IEVOLVE_DEPENDENCY_TARGETS}
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND ""
  )

  if(BUILD_TESTING)
    foreach(component IN LISTS IEVOLVE_COMPONENTS)
      add_test(NAME ievolve.${component}
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir
          "${CMAKE_CURRENT_BINARY_DIR}/ievolve/ievolve/${component}"
          -C $<CONFIG> --output-on-failure --no-tests=error)
      set_tests_properties(ievolve.${component} PROPERTIES LABELS "${component}")
    endforeach()
  endif()

  return()
endif()

find_package(absl CONFIG REQUIRED)
find_package(nlohmann_json 3.12 CONFIG REQUIRED)
find_package(yaml-cpp 0.8 CONFIG REQUIRED)

if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)
endif()

add_subdirectory(ievolve)
