# yaml-cpp 0.8.0 relies on a transitive <cstdint> include removed in GCC 15.
# Backport the missing include: https://github.com/jbeder/yaml-cpp/issues/1307.
set(emitter_utils "${SOURCE_DIR}/src/emitterutils.cpp")
file(READ "${emitter_utils}" contents)
if(NOT contents MATCHES "#[ \t]*include[ \t]*[<\"](cstdint|stdint.h)[>\"]")
  file(WRITE "${emitter_utils}" "#include <cstdint>\n${contents}")
endif()
