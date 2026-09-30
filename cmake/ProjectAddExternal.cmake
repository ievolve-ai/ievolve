include_guard(GLOBAL)
include(ExternalProject)

# A single entry point for pinned third-party CMake projects. Local source
# overrides still use ExternalProject_Add for the configure/build/install steps.
function(Project_AddExternal name)
  cmake_parse_arguments(PARSE_ARGV 1 dep "" "URL;SHA256" "CMAKE_ARGS")
  if(dep_UNPARSED_ARGUMENTS OR NOT dep_URL OR NOT dep_SHA256)
    message(FATAL_ERROR "Project_AddExternal(${name}) requires URL and SHA256")
  endif()
  string(TOUPPER "${name}" upper_name)
  set(IEVOLVE_${upper_name}_SOURCE_DIR "" CACHE PATH "Offline ${name} source tree")
  if(IEVOLVE_${upper_name}_SOURCE_DIR)
    set(source_args SOURCE_DIR "${IEVOLVE_${upper_name}_SOURCE_DIR}"
      DOWNLOAD_COMMAND "")
  else()
    set(archive_url "${dep_URL}")
    if(IEVOLVE_DEPENDENCY_ARCHIVE_DIR)
      set(archive_url "${IEVOLVE_DEPENDENCY_ARCHIVE_DIR}/${name}.tar.gz")
    endif()
    set(source_args URL "${archive_url}" URL_HASH "SHA256=${dep_SHA256}"
      DOWNLOAD_EXTRACT_TIMESTAMP FALSE TLS_VERIFY TRUE)
  endif()
  ExternalProject_Add(${name}_external
    LIST_SEPARATOR "|"
    PREFIX "${CMAKE_BINARY_DIR}/_deps/${name}"
    ${source_args}
    CMAKE_ARGS ${IEVOLVE_COMMON_CMAKE_ARGS}
      -DCMAKE_INSTALL_PREFIX=${IEVOLVE_DEPENDENCY_PREFIX}
      -DCMAKE_INSTALL_LIBDIR=lib
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_TESTING=OFF
      ${dep_CMAKE_ARGS}
    UPDATE_COMMAND ""
  )
endfunction()
