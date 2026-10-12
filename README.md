# ievolve

An LLM-driven evolutionary program optimizer in C++17.

Give it a single-file CLI seed program and an independent CLI evaluator. It repeatedly asks a language model to improve
the program, checks correctness before scoring, and keeps a population of valid results.
Python and C++ candidates and evaluators can be combined independently.
Progress is checkpointed, so a run can be interrupted and resumed.

The model is reached through an installed **Claude Code** or **Codex** CLI using that CLI's existing
login. There is no HTTP client and no API key handling.

---

## The evolution loop

```
seed program ──► evaluate ──► admit as iteration 0
                                   │
        ┌──────────────────────────┘
        ▼
   select island ──► sample parent + inspirations ──► build prompt
                                                          │
                                                          ▼
   admit / reject ◄── evaluate candidate ◄── apply edit ◄── model
        │
        └──► checkpoint every N iterations ──► repeat until a stop condition
```

A run stops on one of four conditions, reported as `stop_reason`: `iteration_limit`,
`target_score`, `early_stopping`, or `requested` (SIGINT/SIGTERM).

Each iteration either **accepts** a candidate into the population or **rejects** it. A rejected
candidate still consumes one iteration of the budget. Invalid edits (empty code, excessive
length, invalid UTF-8) are rejected before evaluation. Build failures and failed correctness
checks are rejected during evaluation. Neither enters the database; valid negative scores
remain eligible. A rejected seed stops the run before population initialization or model calls.

---

## Requirements

| | |
|---|---|
| CMake | 3.24 or newer |
| Compiler | any C++17 compiler |
| Python | only for Python candidates/evaluators and the offline test fixtures |
| Model CLI | a logged-in `claude` or `codex`, only when iterations > 0 |

Platforms: macOS and Linux. Process execution on Windows returns `Unimplemented`.

The first configure downloads pinned dependencies; later builds are incremental and offline.

---

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

Presets do the same into `build/<preset>/`:

```sh
cmake --preset debug && cmake --build --preset debug -j 8 && ctest --preset debug
```

`debug` builds with tests, `release` builds libraries and the CLI without tests, and `asan`
adds AddressSanitizer and UndefinedBehaviorSanitizer to this project's own sources.

### The two build layers

The root `CMakeLists.txt` is a **superbuild**. It builds the pinned third-party libraries, then
re-invokes itself with `IEVOLVE_SUPERBUILD=OFF` into `build/ievolve/`. This matters in practice:

| | Outer (`build/`) | Inner (`build/ievolve/`) |
|---|---|---|
| CTest runs | one forwarding test per component, named `ievolve.<component>` | the real test cases |
| CLI binary | — | `build/ievolve/bin/ievolve` |
| `compile_commands.json` | — | the one to point clangd at |

Use the inner directory while developing. `-DIEVOLVE_BUILD_EXAMPLES=ON` also builds
`bin/evaluator`, the C++ sorting evaluator. Test builds include it as an integration fixture.

### Library-only, offline, and dependency overrides

```sh
# No tests; GoogleTest is not downloaded.
cmake -S . -B build-release -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release

# Fully offline. The directory must hold abseil.tar.gz, json.tar.gz, yaml_cpp.tar.gz and
# googletest.tar.gz matching the SHA256s in cmake/Dependencies.cmake.
cmake -S . -B build -DIEVOLVE_DEPENDENCY_ARCHIVE_DIR=/path/to/archives
```

`IEVOLVE_ABSEIL_SOURCE_DIR`, `IEVOLVE_JSON_SOURCE_DIR`, `IEVOLVE_YAML_CPP_SOURCE_DIR` and
`IEVOLVE_GOOGLETEST_SOURCE_DIR` point at already-extracted sources instead; version compatibility
is then the caller's responsibility. When reusing an existing build directory, re-pass any custom
`IEVOLVE_*` variables.

| Dependency | Version | Used for |
|---|---|---|
| Abseil | 20250127.1 | `Status`/`StatusOr`, string utilities |
| nlohmann/json | 3.12.0 | metrics, artifacts, configuration |
| yaml-cpp | 0.8.0 | YAML configuration parsing and output |
| GoogleTest | 1.16.0 | unit tests, only when `BUILD_TESTING=ON` |

The superbuild adds yaml-cpp 0.8.0's missing `<cstdint>` include for GCC 15 compatibility.
This also applies to offline source overrides; sources that already include it are left unchanged.

---

## Test

```sh
# Everything.
ctest --test-dir build/ievolve --output-on-failure

# One component: utils config program prompt process llm code database evaluator controller cli
ctest --test-dir build/ievolve -L '^llm$' --output-on-failure

# One test case.
ctest --test-dir build/ievolve -R 'PromptSamplerTest' --output-on-failure
```

Prefer `ctest -R` over running a test binary directly — several tests depend on environment
variables and compile definitions that CTest supplies.

Formatting follows `.clang-format` (Google style, C++17, 120-column line limit):

Keep blank lines between logical blocks. Use braces for multiline `if` and `else` branches;
only an `if` whose condition and entire statement fit on one line may omit them.

```sh
clang-format --dry-run --Werror \
  ievolve/{utils,config,program,prompt,llm,code,database,process,evaluator,controller,cli}/*.h \
  ievolve/{utils,config,program,prompt,llm,code,database,process,evaluator,controller,cli}/*.cc
```

---

## Run

```sh
ievolve --config config.yaml
```

YAML is the only source of runtime settings. The command line accepts only
`-c, --config FILE`, `-h, --help`, and `--version`; `--config` is required for a run.
Old positional arguments and runtime override flags are rejected.

Put input/output paths and checkpoint under `run:`, and build/run commands under
`candidate:` and `evaluator:`. Keep the iteration budget in `max_iterations`, logging in
`log_level`, and model selection in `llm.models`. `run.base_directory` defaults to the YAML
directory; relative file paths use that base, including the default output `ievolve_output`.
A relative executable containing a directory uses the same base; bare names use `PATH`.

`max_iterations` is the total number of attempts for the whole run, including those already in a
resumed checkpoint: resuming `checkpoint_5` with `max_iterations: 10` runs five more, and a
checkpoint at or past the budget runs none. Set it to `0` to
evaluate only the seed (with `evaluator.use_llm_feedback: false`, no models are called).
Set `run.checkpoint` to resume; the initial program file is then optional and is never read.

Language is inferred from the initial program's suffix for a fresh run, or from the checkpoint's
`checkpoint.json` program metadata on resume. Both must use a recognized language extension.
If `language` is configured, it must exactly match the inferred name (for example, `.py` requires
`python`, including case); otherwise it is filled from that inference. Checkpoint metadata must
also agree with the suffix. Mismatches fail before model or evaluator creation.

The output `file_suffix` defaults to the source suffix. An explicit value must identify the same
language: `.cc` to `.cpp` is allowed, while `.py` to `.rs`, `.code`, and an empty suffix are rejected.
On resume the configured suffix must exactly match the saved suffix, including aliases;
restoration does not depend on any exported best-program filename.

**Exit codes:** `0` completed, `1` runtime failure, `2` invalid usage or configuration,
`130`/`143` on SIGINT/SIGTERM. A signal requests a stop after the current operation finishes,
so progress is checkpointed before exiting.

**Progress log:** the run reports its state on stderr, filtered by `log_level`:

```text
[INFO] Starting evolution (iterations=10, output=ievolve_output)
[INFO] Evaluating initial program
[INFO] Initial program score 0.9597642169962064, target 2.635
[INFO] Iteration 1/10 accepted: score 2.2104578311406155, target 2.635 (38.2s)
[INFO] New best program iteration-1 with score 2.2104578311406155, target 2.635
[INFO] Iteration 2/10 rejected: No valid diffs found in response (21.7s)
[INFO] Saved checkpoint ievolve_output/checkpoints/checkpoint_10
[INFO] Evolution finished after 10 iteration(s); best program iteration-7 with score 2.625219033765203, target 2.635
```

`DEBUG` adds the parent and islands of each attempt and island migrations; a signal is reported
as a `WARNING` as soon as it arrives. Scores are the same fitness used for ranking; `target` appears
only when `run.target_score` is set. A rejected attempt has a reason and no fitness score.

### CLI evaluation contract

`CandidateConfig` describes how to build and launch the program being optimized.
`EvaluatorConfig` describes a fixed judge that supplies test inputs, checks outputs, and
measures performance. Both use optional `build` and required `run` command arrays. The
framework builds the fixed evaluator once per backend and each candidate once per evaluation.
Compilation has its own timeout; it is excluded from the evaluator's runtime budget and
fitness unless your evaluator explicitly defines a different metric.

The framework sends this UTF-8 JSON request to the evaluator's stdin and closes it:

```json
{
  "protocol_version": 1,
  "candidate": {"argv": ["/tmp/candidate-workspace/program"], "cwd": "/tmp/candidate-workspace"},
  "limits": {"evaluation_timeout_ms": 30000, "max_artifact_bytes": 104857600},
  "enable_artifacts": true
}
```

The judge directly launches the supplied `argv` in `cwd`, captures both output streams,
checks all required cases, then measures fitness. Candidate input/output belongs to the
particular task; it need not use the evaluator protocol. The time budget covers the entire
evaluator process, not each test case. Judges define their own case limits, warmups and repeats.

Exit zero and emit exactly one JSON object on stdout, with logs on stderr:

```json
{"protocol_version":1,"passed":true,"metrics":{"combined_score":1.25,"runtime_ms":8.0}}
```

```json
{"protocol_version":1,"passed":false,"reason":"wrong_answer","metrics":{},"artifacts":{"case":"expected 6, got 5"}}
```

A passed result requires a finite numeric `combined_score` (negative values are valid;
booleans and numeric strings are not) and must omit `reason`. Larger scores are better.
A rejected result requires a nonempty UTF-8 reason of at most 4096 bytes and must omit
`combined_score`. Other metrics are finite JSON scalars. The framework validates the verdict
and data contract; the judge implements correctness and must not trust a candidate's claimed score.
Duplicate JSON keys, unknown envelope fields/versions, trailing non-whitespace, and more
than 64 object/array nesting levels are errors.

Optional artifacts are UTF-8 text strings or binary objects such as
`{"__bytes__":"AAEC"}` with canonical base64. Decoded payloads share `max_artifact_storage` (default 100 MiB).
`enable_artifacts: false` discards payloads and rejection diagnostics while retaining
correctness checks. The `ENABLE_ARTIFACTS` environment override is captured at evaluator creation.
Valid programs' artifacts can feed subsequent prompts through `prompt.include_artifacts`.
Optional LLM feedback runs only after correctness passes, and the blended score is validated again.

| Event | Result |
|---|---|
| Candidate build exits nonzero, times out or exceeds output cap | Reject candidate |
| Judge reports wrong answer, candidate crash or per-case limit | Reject candidate |
| Whole evaluator times out or exceeds output cap | Reject evaluation with the limit phase |
| Compiler/judge cannot start; fixed judge build fails | Stop with an infrastructure error |
| Judge exits nonzero or emits malformed protocol | Stop with an infrastructure error |

A missing candidate interpreter is an infrastructure error: the judge exits nonzero. Once
started, a candidate's nonzero exit is a rejection. Evaluations do not retry automatically.
Build exit failures retain bounded compiler diagnostics. On timeout/output-limit errors,
partial process output is unavailable; the phase and reason remain available.

Each candidate gets a temporary source/build directory. The fixed evaluator's build directory
lives for the backend's lifetime; its run command uses that directory's executable but runs
with the candidate workspace as cwd. Fixed data should use `{base_dir}` absolute arguments.
All outer builds and evaluations own a process group and clean it on success as well as failure.
Judges must let candidate processes inherit that group (Python: no `start_new_session=True`;
C++: `ProcessRequest::inherit_process_group = true`). In inherited mode local cleanup reaps
the direct child; the outer framework cleans remaining descendants. Processes that explicitly
detach are outside this lifecycle guarantee.

### Sorting across four language combinations

[`examples/cli_sorting/`](examples/cli_sorting/README.md) includes Python/Python, Python/C++,
C++/Python and C++/C++ configurations. They default to zero model iterations:

```sh
build/ievolve/bin/ievolve --config examples/cli_sorting/config_cpp_cpp.yaml
```

The C++ judge uses the already installed dependencies from `build/_deps/install`. Its build
script runs entirely in the evaluator workspace. See the example README for an alternate
prefix, all configurations and the exact timing method. The C++/C++ runtime does not invoke Python.

### Worked example

`examples/circle_packing/` packs 26 non-overlapping circles into a unit square, maximizing the
sum of radii.

```sh
python3 -m pip install -r examples/circle_packing/requirements.txt
build/ievolve/bin/ievolve --config examples/circle_packing/config.yaml
# Or use the wrapper, which builds the binary on the first run:
./run_example.sh
```

Before running, edit `examples/circle_packing/config.yaml`: set both `candidate.run.argv[0]`
and `evaluator.run.argv[0]` to a Python with NumPy installed, `run.output_directory` to a fresh
destination, and `max_iterations` to the desired budget (`0` for baseline; the default is `100`). To select Claude Code,
set the model's `provider` in `llm.models` to `claude_code`. For resume, set `run.checkpoint`,
for example `../../build/packing_2/checkpoints/checkpoint_3`, and set `max_iterations` to the
total budget (resumed iterations count toward it).

The wrapper forwards only application arguments and does not override YAML, delete earlier
results, or automatically recheck geometry. Recheck a saved program with `check_packing.py`.

See [`examples/circle_packing/README.md`](examples/circle_packing/README.md) for recorded results
and an independent geometry check.

### Output layout

```
<output>/
├── best/
│   ├── best_program<suffix>          # best code found so far
│   └── best_program_info.json        # id, generation, metrics, language
├── checkpoints/
│   └── checkpoint_<N>/
│       ├── database/                 # full population, archive, RNG state
│       └── checkpoint.json           # schema, language, counters and stopping policy
└── artifacts/                        # only for artifacts too large to inline
```

Artifacts at or under `database.artifact_size_threshold` are stored inline with the program, so
`artifacts/` appears only once something larger is produced. Set `database.artifacts_base_path`
to put it elsewhere.

A checkpoint directory is published by a single atomic rename, so a reader never sees a partial
one. The checkpoint is the authoritative record for resuming; `best/` is a convenience copy.
Rejected candidates are not saved. Their reason is logged at INFO; with `log_level: debug`, each
evaluation diagnostic (build and evaluator stderr, evaluator artifacts) follows, truncated to
4096 bytes. The rejected source is never logged.

`checkpoint.json` uses `format: "ievolve.cli-checkpoint"`, `schema_version: 1`, and
`evaluation_protocol_version: 1`. It records `program` (language, suffix, best ID), `progress`
(iteration, accepted/rejected counts, early-stopping state), and `stopping_policy`. The database
contains complete source, valid metrics, artifacts, population, archive, and RNG state. Restore
validates all fields, references, counts, scores, artifacts and configuration before changing
in-memory state. Missing fields, unsupported versions and old layouts are rejected without repair.

Neither the initial file nor the exported best file is required on resume. Temporary directories
and binaries are not saved; the fixed evaluator is rebuilt from the current configuration.
Keep test cases and scoring semantics unchanged when resuming: compiler/dependency/evaluator
fingerprints are not recorded, and timing or external LLM results are not guaranteed reproducible.

---

## Architecture

Static libraries under `ievolve/` are exported as `ievolve::<name>`. Dependencies flow strictly
one way — there are no cycles.

```
          config_types ────────────────┐   (header-only shared types)
                                       │
  program ──┬──► prompt ───────────────┼──────────────┐
            │                          │              │
            └──► code ──► database ────┤              │
                                       │              ▼
  process ─────► llm ──────────────────┴──► evaluator ──► controller ──► cli
                                                             ▲
  config ────────────────────────────────────────────────────┘
```

| Component | Responsibility |
|---|---|
| `utils` | shared text helpers, thread joining, leveled logging; separate YAML helper target |
| `config` | configuration types, YAML/JSON load, validation, model overrides |
| `program` | the shared `Program` value model, JSON conversion, fitness |
| `prompt` | templates, fragments, artifact rendering, prompt sampling |
| `process` | subprocess execution, timeouts, process-group cleanup, output caps |
| `llm` | `LLMInterface`, Claude Code and Codex CLI clients, weighted ensemble |
| `code` | diff extraction, full-rewrite parsing, EVOLVE-BLOCK enforcement |
| `database` | MAP-Elites population, islands, archive, checkpoints, artifacts |
| `evaluator` | command builds/workspaces, CLI protocol, explicit verdicts, optional LLM feedback |
| `controller` | one iteration end to end, plus the serial main loop |
| `cli` | argument parsing, dependency assembly, signal handling |

### Layering rules the code enforces

- `prompt` links only `config_types`, never the YAML parser. `config_types` is a header-only
  INTERFACE target precisely so that stays true.
- `database` depends on the program, code and config *types* only — never on prompt, LLM,
  YAML or Python.
- `controller` splits in two. `IterationRunner` owns a single attempt (prompt → model → edit →
  evaluate) and deliberately holds **no mutable database**. `Controller` owns everything stateful:
  parent selection, ID allocation, admission, prompt logging, checkpoints and stop conditions.

### Cross-cutting conventions

**Errors.** Recoverable failures return `absl::Status` / `absl::StatusOr`; callers check `.ok()`
before dereferencing. Diagnostics carry field locations, never configuration values, prompts or
raw model output.

**Transactional state.** Every population change runs through `ProgramDatabase::Mutate`, which
applies the change to a full copy and swaps it in only on success — programs, cells, archive,
feature statistics, counters and RNG roll back together. The database never writes to disk as a
side effect: callers save and load it with an explicit path, as the controller's checkpoints do.
`Controller::Step` uses the same copy-and-commit shape for the database and its progress counters.

**Embedded resources.** Prompt templates (`ievolve/prompt/defaults/`) are embedded at CMake
configure time. Evaluation uses only the configured CLI commands; there is no embedded Python
worker. Editing prompt templates triggers a re-configure.

**Model access.** `ievolve::llm` shells out to the installed CLI with its own login. Calls use a
read-only sandbox with tools disabled and a fresh empty working directory. Arguments are passed via
argv and stdin with no shell involved; combined stdout and stderr are capped (8 MiB by default), and
a timeout kills the whole process group.

**Reproducibility.** `random_seed` makes runs reproducible under one C++ standard library.
Sampling advances RNG state that is persisted in checkpoints, so a resumed run continues the same
stream rather than replaying it.

### File layout

Each unit is a `name.h` / `name.cc` / `name_test.cc` triple, colocated, in namespace `ievolve`
(`ievolve::controller` and `ievolve::evaluator` for those two). Golden fixtures live in each
component's `utest_data/`.

```
ievolve/
├── CMakeLists.txt        # registers every component
├── cli/  code/  config/  controller/  database/
├── evaluator/  llm/  process/  program/  prompt/
cmake/                    # pinned dependencies and superbuild helpers
docs/                     # local design notes and validation records (ignored by Git)
examples/cli_sorting/     # four candidate/evaluator language combinations
examples/circle_packing/  # CLI geometry example with historical recorded results
tools/                    # fixture generators
```

---

## Configuration

Pass a YAML file with `--config`, containing either `run.initial_program` or `run.checkpoint`
and both `candidate.run.argv` and `evaluator.run.argv`. `run`, `candidate`, `evaluator` and
nested command objects reject unknown keys. Unchanged algorithm sections retain their Python
reference behavior for unknown keys. Library config users may leave commands unassembled;
the production backend requires them before execution.

`Config::Load` resolves `run.base_directory` against the YAML location, then resolves source,
output, checkpoint and template paths against that base. Saving preserves its absolute value,
so moving a saved configuration does not silently change fixed resources. `FromJson`/`ParseYaml`
preserve paths as supplied; their callers must provide an absolute existing base directory.

Commands are argv arrays, never implicit shell text. Each argument expands once:
`{source}` is the candidate source or `evaluator.source`; `{workspace}` is that program's build
workspace; `{base_dir}` is the fixed base directory. `{{` and `}}` escape literal braces.
Unknown/unbalanced placeholders are errors, as is using evaluator `{source}` without a source.
Substituted spaces, braces and shell characters stay in the original single argument.
Only relative `argv[0]` with a directory is rebased; other arguments need explicit placeholders.
Invoke a build script explicitly when additional include/link arguments or multiple commands are needed.

```yaml
run:
  initial_program: initial_program.py # optional when resuming
  base_directory: .                 # relative to this YAML file; persisted as absolute
  output_directory: ievolve_output  # relative to base_directory
  checkpoint: null                   # checkpoint directory to resume
  target_score: null                 # stop at numeric combined_score >= this value
  codex_executable: codex
  claude_executable: claude

max_iterations: 10000
log_level: INFO                       # DEBUG, INFO, WARNING, ERROR or CRITICAL
checkpoint_interval: 100
random_seed: 42
language: python              # inferred from the seed file extension when omitted
file_suffix: .py              # must identify the same language; inferred when omitted
diff_based_evolution: true    # false asks for a complete rewrite each time
max_code_length: 10000
enforce_evolve_blocks: false  # restrict edits to EVOLVE-BLOCK regions
early_stopping_patience: null # accepted children without improvement before stopping
convergence_threshold: 0.001
early_stopping_metric: combined_score

llm:
  timeout: 60
  retries: 3
  models:
    - provider: claude_code   # or codex
      name: sonnet            # omit to let the CLI pick its default
      weight: 1.0
  evaluator_models: []        # inherits `models` when empty

prompt:
  template_dir: null          # relative paths resolve against the config file
  num_top_programs: 3
  num_diverse_programs: 2
  include_artifacts: true
  max_artifact_bytes: 20480

database:
  population_size: 1000
  archive_size: 100
  num_islands: 5
  exploration_ratio: 0.2      # exploration + exploitation must not exceed 1.0
  exploitation_ratio: 0.7
  feature_dimensions: [complexity, diversity]
  feature_bins: 10            # a scalar floor, or a per-dimension map
  migration_interval: 50
  migration_rate: 0.1

candidate:
  run:
    argv: [python3, '{source}']
  # For C++: set initial_program to a .cc/.cpp file, use language: cpp,
  # file_suffix: .cpp, add build below, and run ['{workspace}/candidate'].
  # build:
  #   argv: [c++, '-std=c++17', '-O2', '{source}', '-o', '{workspace}/candidate']
  #   timeout: 60
  #   max_output_bytes: 8388608

evaluator:
  source: evaluator.py              # optional for a prebuilt evaluator executable
  run:
    argv: [python3, '{source}']
  # Optional build has the same fields/defaults as candidate.build.
  timeout: 300                       # seconds for the entire evaluator, excluding builds
  max_output_bytes: 8388608          # stdout + stderr
  max_artifact_storage: 104857600    # decoded payload bytes
  use_llm_feedback: false
  enable_artifacts: true
```

Model weights select which model handles each generation; they are normalized, so relative values
are what matter.

---

## Embedding in another project

Run the superbuild once, add its `_deps/install` to `CMAKE_PREFIX_PATH`, then:

```cmake
set(IEVOLVE_SUPERBUILD OFF)
add_subdirectory(ievolve)
target_link_libraries(your_target PRIVATE ievolve::prompt ievolve::config)
```

For YAML conversion without `Config`, include `ievolve/utils/yaml.h` and link
`ievolve::utils_yaml`. Its `ievolve::utils` functions parse and emit ordered JSON,
and read and write YAML files. Configuration normalization and validation remain
in `Config`. The separate target keeps yaml-cpp out of `ievolve::utils` dependencies.

For logging, include `ievolve/utils/log.h` and link `ievolve::utils`:

```cpp
#include "ievolve/utils/log.h"

ievolve::utils::Logger logger;  // INFO and above to standard error
logger.SetLevel(ievolve::utils::LogLevel::kDebug);
logger.Debug("iteration=", 3, ", score=", 1.5);
logger.Info("Starting evolution");
logger.Warning("Candidate rejected");
logger.Error("Evaluation failed");
logger.Critical("Cannot continue");
```

Messages use `[LEVEL] message` followed by a newline and are flushed immediately.
Levels are `DEBUG`, `INFO`, `WARNING`, `ERROR`, and `CRITICAL`; the threshold includes
its own level and all higher levels. `Critical()` logs without terminating the process.
`Logger(stream, level)` can write to a caller-owned stream, including an open `std::ofstream`;
the stream must outlive the logger and its calls. Logger instances keep independent thresholds
and serialize their writes, even when sharing a stream. Filtered messages skip formatting.
The CLI uses YAML `log_level` for progress messages; command failures always report their exit reason.

```cpp
#include "ievolve/config/config.h"
#include "ievolve/controller/controller.h"
#include "ievolve/llm/ensemble.h"

auto config = ievolve::Config::Load("config.yaml");
if (!config.ok()) return;
// Set language and file_suffix in config.yaml before creating a controller.

auto ensemble = ievolve::LLMEnsemble::Create(config->llm.models);
if (!ensemble.ok()) return;

ievolve::evaluator::EvaluatorOptions evaluation_options;
evaluation_options.file_suffix = config->file_suffix;
auto evaluator = ievolve::evaluator::Evaluator::Create(
    config->candidate, config->evaluator, *config->run.base_directory, evaluation_options);
if (!evaluator.ok()) return;

// max_iterations, run.output_directory, run.target_score and run.checkpoint
// all come from the config; ControllerOptions holds only runtime hooks.
ievolve::controller::ControllerOptions settings;
auto controller = ievolve::controller::Controller::Create(
    *config, std::move(*ensemble), std::move(*evaluator), settings);
if (!controller.ok()) return;

auto result = (*controller)->Run({seed_code});
if (!result.ok()) return;
// result->best, result->stop_reason, result->checkpoint_path
```

Each `Controller` instance runs once; to resume, set `run.checkpoint` and create a new one.
`InitialProgram` contains only code; the controller takes the language from `Config::language`,
which must be set before `Controller::Create`. The CLI handles inference and matching before
constructing the controller; library callers supply the resolved language themselves.
`PopulationStrategy` accepts callbacks for admission, cell replacement, archiving, eviction and
migration if you want to override the defaults.

---

## Limitations

- **Serial only.** The controller runs one iteration at a time. Parallel workers are not
  implemented.
- **Explicitly unsupported**, returning `Unimplemented` rather than degrading silently: evolution
  tracing and worker recycling.
- **Single source files.** Multi-file C++ project evolution is not implemented. Fixed evaluator
  scripts can compile/link dependencies, but only the candidate source file is evolved.
- **Intentional interface break.** Python module `evaluate(path)`/stage entry points and the
  embedded worker have been removed. Use CLI protocol v1. Old `run.evaluation_file`,
  `run.python_executable`, `evaluator.max_retries`, cascade/resource/distributed/parallel keys
  are configuration errors. Checkpoints support only the new complete schema; no legacy import
  or missing-field repair is provided. Python golden fixtures cover unchanged configuration
  and algorithm behavior, with the removed execution fields excluded from comparisons.
- **`max_iterations` is a total budget.** Python runs `max_iterations` additional attempts after a
  resumed checkpoint; ievolve counts the resumed attempts toward it, so interrupting and resuming
  with the same configuration runs the same number of attempts as an uninterrupted run.
- **The database lives in memory.** Python's `database.in_memory` is not part of the schema (Python
  never reads it either) and is ignored like any unknown `database` key. Progress is persisted only
  through the controller's checkpoints; the database is not saved after every change.
- **Dropped Python database settings.** `db_path` (resume with `run.checkpoint`; artifacts go to
  `artifacts_base_path`, which the CLI defaults to `<output_directory>/artifacts`),
  `diversity_metric` (Python never reads it; diversity is always edit distance) and
  `max_snapshot_artifacts` (only used by parallel worker snapshots) are not part of the schema
  either and are ignored.
- **No embedding novelty check, by design.** OpenEvolve can reject candidates whose embedding is
  too similar to an island member and ask `novelty_llm` to judge them. That needs an
  OpenAI-compatible embedding endpoint and API key, while ievolve uses only the `claude_code` and
  `codex` CLIs, so the feature is not ported. `embedding_model`, `embedding_api_base`,
  `similarity_threshold` and `novelty_llm` are not part of the schema: like any unknown `database`
  key they are ignored, and every candidate is treated as novel.
- **Not a sandbox.** Timeouts kill the process group, but evaluation scripts and generated code run
  with your privileges.
- **No durability guarantees.** Checkpoints are published atomically but are not fsynced, and
  concurrent writers to one output directory are not supported.
- **API-style model parameters** (`temperature`, `top_p`, `max_tokens`) have no CLI equivalent.
  They are accepted and preserved, but unused.

---

## License

MIT — see [LICENSE](LICENSE).

The bundled prompt templates in `ievolve/prompt/defaults/` originate from OpenEvolve and retain
their upstream MIT license; see `ievolve/prompt/defaults/LICENSE.openevolve`.
