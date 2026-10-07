# Single-file CLI sorting

These examples optimize one source file. The candidate reads `N` followed by
`N` signed integers from stdin, then writes exactly those integers in ascending
order to stdout. It supplies no score. Both candidates start with insertion sort.

Build ievolve from the repository root first:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 8
```

Run any combination with the same framework:

```sh
build/ievolve/bin/ievolve --config examples/cli_sorting/config_cpp_cpp.yaml
build/ievolve/bin/ievolve --config examples/cli_sorting/config_cpp_python.yaml
build/ievolve/bin/ievolve --config examples/cli_sorting/config_python_cpp.yaml
build/ievolve/bin/ievolve --config examples/cli_sorting/config_python_python.yaml
```

Each configuration uses `max_iterations: 0`: it builds and evaluates the seed,
exports the best source and saves a checkpoint without model calls. Outputs go
to `build/cli_sorting_smoke/<candidate>_<evaluator>`. For another run, choose a new
`run.output_directory` or explicitly resume from the saved checkpoint.

C++ candidates require a C++17 compiler (`c++` in these configurations). Python
programs require Python 3.9+ without additional packages. Edit the respective
`run.argv[0]` to select an interpreter; commands are argument arrays. C++/C++
does not start or require Python at runtime.

The C++ evaluator uses ievolve's process library and nlohmann JSON. Its build
script configures a separate CMake tree inside the provided evaluator workspace
and uses the already installed `build/_deps/install` dependencies. It downloads
nothing and does not build into the shared project build tree. If your dependency
prefix differs, append it as the fourth element of `evaluator.build.argv`.
The script interface is:

```sh
sh examples/cli_sorting/build_evaluator.sh /absolute/build/workspace /absolute/dependency/prefix
```

For a direct non-testing build, enable `-DIEVOLVE_BUILD_EXAMPLES=ON` and build
`ievolve_cli_sorting_evaluator`. Offline tests also build this executable as a
fixture. Its output is `bin/evaluator` inside that CMake build directory.

Both evaluators first check seven cases: empty, singleton, unsorted, duplicate
and negative values, sorted, reverse sorted, and identical deterministic random
values. After correctness passes, one warmup and five timed executions use the
random case. `runtime_ms` is the mean process wall time, including startup and
I/O. `combined_score = 1 / max(runtime_ms, 0.001)`, so higher is better. For small
inputs startup overhead dominates; use larger workloads or a persistent CLI
protocol when measuring algorithm throughput becomes the goal.

Each case has a two-second limit and a combined stdout/stderr limit of 1 MiB.
Wrong answers, candidate nonzero exits and local limits produce `passed: false`
without fitness. A missing executable or malformed evaluator request is an
infrastructure error and stops the run. Logs go to stderr. The framework builds
C++ candidates in separate workspaces and builds the fixed evaluator once.

To evolve beyond the seed, set a positive `max_iterations` and configure
`llm.models` with your chosen supported model client. Keep the CLI contract in
the prompt. Rejection reasons are logged; with `log_level: debug` and artifacts
enabled, bounded build and evaluator diagnostics are logged as well.

The evaluator request/response protocol is described in the repository README.
Candidates inherit the evaluator process group, allowing the framework to clean
up descendants on timeout or evaluator exit. Deliberately detached processes
are outside this lifecycle guarantee; it is not a sandbox.
