#ifndef IEVOLVE_LLM_PROCESS_H_
#define IEVOLVE_LLM_PROCESS_H_

#include "ievolve/process/process.h"

// Compatibility aliases for users of the original LLM process header.
namespace ievolve {
using process::ProcessRequest;
using process::ProcessResult;
using process::ProcessRunner;
using process::RunProcess;
}  // namespace ievolve

#endif  // IEVOLVE_LLM_PROCESS_H_
