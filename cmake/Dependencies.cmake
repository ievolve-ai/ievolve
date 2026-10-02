include("${CMAKE_CURRENT_LIST_DIR}/ProjectAddExternal.cmake")
set(IEVOLVE_DEPENDENCY_PREFIX "${CMAKE_BINARY_DIR}/_deps/install")
set(IEVOLVE_DEPENDENCY_ARCHIVE_DIR "" CACHE PATH
  "Offline archives named abseil.tar.gz, json.tar.gz, yaml_cpp.tar.gz, googletest.tar.gz")
set(IEVOLVE_COMMON_CMAKE_ARGS
  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  -DCMAKE_CXX_STANDARD=17
  -DCMAKE_CXX_STANDARD_REQUIRED=ON
  -DCMAKE_CXX_EXTENSIONS=OFF
  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)
foreach(variable CMAKE_TOOLCHAIN_FILE CMAKE_OSX_ARCHITECTURES
    CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT)
  if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
    string(REPLACE ";" "|" encoded_value "${${variable}}")
    list(APPEND IEVOLVE_COMMON_CMAKE_ARGS "-D${variable}=${encoded_value}")
  endif()
endforeach()

Project_AddExternal(abseil
  URL https://codeload.github.com/abseil/abseil-cpp/tar.gz/refs/tags/20250127.1
  SHA256 b396401fd29e2e679cace77867481d388c807671dc2acc602a0259eeb79b7811
  CMAKE_ARGS -DABSL_ENABLE_INSTALL=ON -DABSL_BUILD_TESTING=OFF
    -DABSL_PROPAGATE_CXX_STD=ON)
Project_AddExternal(json
  URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
  SHA256 4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
  CMAKE_ARGS -DJSON_BuildTests=OFF -DJSON_Install=ON)
Project_AddExternal(yaml_cpp
  URL https://codeload.github.com/jbeder/yaml-cpp/tar.gz/refs/tags/0.8.0
  SHA256 fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
  CMAKE_ARGS -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF
    -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_CPP_INSTALL=ON
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
ExternalProject_Add_Step(yaml_cpp_external fix_missing_cstdint
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=<SOURCE_DIR>
    -P "${CMAKE_CURRENT_LIST_DIR}/PatchYamlCpp.cmake"
  DEPENDEES patch
  DEPENDERS configure
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/PatchYamlCpp.cmake")
set(IEVOLVE_DEPENDENCY_TARGETS abseil_external json_external yaml_cpp_external)
if(BUILD_TESTING)
  Project_AddExternal(googletest
    URL https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.16.0
    SHA256 78c676fc63881529bf97bf9d45948d905a66833fbfa5318ea2cd7478cb98f399
    CMAKE_ARGS -DBUILD_GMOCK=OFF -DINSTALL_GTEST=ON
      -Dgtest_force_shared_crt=ON)
  list(APPEND IEVOLVE_DEPENDENCY_TARGETS googletest_external)
endif()
