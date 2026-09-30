# ievolve

An LLM-driven evolutionary program optimizer in C++17.

Give it a seed program and an evaluation script. It repeatedly asks a language model to improve
the program, scores every candidate with your script, and keeps a population of the best results.
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
candidate still consumes one iteration of the budget: the model produced something unusable
(no valid edit, empty code, over the length limit, invalid UTF-8), so it is never evaluated
and never enters the database.

---

## Requirements

| | |
|---|---|
| CMake | 3.24 or newer |
| Compiler | any C++17 compiler |
| Python | 3.9+ — runs your evaluation script |
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

Use the inner directory while developing.

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

Put input/output paths, checkpoint and executable selection under `run:`. Keep the iteration
budget in `max_iterations`, logging in `log_level`, and model selection in `llm.models`.
Relative `run` paths resolve against the YAML file's directory, including the default output
folder `ievolve_output`. A relative executable with a directory (such as `./bin/python3`) uses
the same base; a bare name such as `python3` is looked up on `PATH`.

`max_iterations` counts additional attempts for both fresh and resumed runs. Set it to `0` to
evaluate only the seed (with `evaluator.use_llm_feedback: false`, no models are called).
Set `run.checkpoint` to resume; the initial program file is then optional and is never read.

Language is inferred from the initial program's suffix for a fresh run, or from the checkpoint's
`best_program<suffix>` filename on resume. Both must use a recognized language extension.
If `language` is configured, it must exactly match the inferred name (for example, `.py` requires
`python`, including case); otherwise it is filled from that inference. Checkpoint metadata must
also agree with the suffix. Mismatches fail before model or evaluator creation.

The output `file_suffix` defaults to the source suffix. An explicit value must identify the same
language: `.cc` to `.cpp` is allowed, while `.py` to `.rs`, `.code`, and an empty suffix are rejected.
This keeps checkpoints produced by the CLI usable for suffix-based language inference on resume.

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
only when `run.target_score` is set. A rejected attempt was never evaluated, so it has no score.

### The evaluation script

Your script defines `evaluate(candidate_file_path)` and returns either a dict of scalar metrics,
or an object with `metrics` and `artifacts`. It is responsible for importing, compiling or running
the candidate; ievolve owns the temporary files, the subprocess lifetime and the result validation.

```python
def evaluate(program_path):
    # import / run / measure the candidate at program_path
    return {"combined_score": 0.87, "runtime_ms": 12.4}
```

`combined_score` is the metric used for ranking, `run.target_score` and early stopping. Without it,
ranking falls back to the mean of the numeric metrics. When present it must be an `int` or `float`;
unlike OpenEvolve, a numeric string such as `"0.87"` is rejected as an invalid result.

Optional `evaluate_stage1`, `evaluate_stage2` and `evaluate_stage3` enable cascade evaluation:
cheap stages run first and a candidate must clear `cascade_thresholds` to reach the next one.
Each stage runs in a fresh process, so module-level state does not carry across stages.

Artifacts returned alongside metrics (text or bytes) are stored with the program and can be fed
back into the next prompt via `prompt.include_artifacts`.

### Worked example

`examples/circle_packing/` packs 26 non-overlapping circles into a unit square, maximizing the
sum of radii.

```sh
python3 -m pip install -r examples/circle_packing/requirements.txt
build/ievolve/bin/ievolve --config examples/circle_packing/config.yaml
# Or use the wrapper, which builds the binary on the first run:
./run_example.sh
```

Before running, edit `examples/circle_packing/config.yaml`: set `run.python_executable` to the
Python with NumPy installed, `run.output_directory` to the destination, and `max_iterations`
to the desired budget (`0` for the baseline; the example defaults to `3`). To select Claude Code,
set the model's `provider` in `llm.models` to `claude_code`. For resume, set `run.checkpoint`,
for example `../../build/packing/checkpoints/checkpoint_3`, and choose the additional budget.

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
│       ├── controller.json           # counters and stopping state
│       └── best_program*             # same pair, as of this checkpoint
└── artifacts/                        # only for artifacts too large to inline
```

Artifacts at or under `database.artifact_size_threshold` are stored inline with the program, so
`artifacts/` appears only once something larger is produced. Set `database.artifacts_base_path`
to put it elsewhere.

A checkpoint directory is published by a single atomic rename, so a reader never sees a partial
one. The checkpoint is the authoritative record for resuming; `best/` is a convenience copy.

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
| `evaluator` | Python evaluation backend, cascade stages, optional LLM feedback |
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
feature statistics, counters and RNG roll back together. In disk mode the checkpoint is published
before the in-memory swap, so a failed write never leaves memory ahead of disk. `Controller::Step`
uses the same copy-and-commit shape for the database and its progress counters.

**Embedded resources.** Prompt templates (`ievolve/prompt/defaults/`) and the Python evaluation
worker are compiled into their libraries at CMake configure time. The runtime needs neither Python
sources nor a template directory. Editing those data files triggers a re-configure.

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
examples/circle_packing/  # runnable example with recorded results
tools/                    # fixture generators
```

---

## Configuration

A YAML file passed with `--config`. `run.evaluation_file` is required, along with either
`run.initial_program` or `run.checkpoint`. Other settings have defaults. Unknown keys under
`run` are errors; the core algorithm schema continues to ignore unknown keys for Python
compatibility. `RunSettings` is defined in `config/types.h` and stored as `Config::run`.
The C++ configuration schema extends the Python reference with this section; `ToJson`,
`ToYaml`, and `Save` all preserve it. `Load` resolves its paths against the YAML directory,
while `FromJson` and `ParseYaml` preserve paths as written. Library users may leave the input
paths unset; the CLI checks that the files needed to start or resume a run are configured.

```yaml
run:
  initial_program: initial_program.py # optional when resuming
  evaluation_file: evaluator.py       # required for fresh and resumed runs
  output_directory: ievolve_output    # relative to this YAML file
  checkpoint: null                   # checkpoint directory to resume
  target_score: null                 # stop at numeric combined_score >= this value
  python_executable: python3
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

evaluator:
  timeout: 300
  max_retries: 3
  cascade_evaluation: true
  cascade_thresholds: [0.5, 0.75, 0.9]
  parallel_evaluations: 1
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
logger.Warning("Retrying evaluation");
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
// Set language in config.yaml before creating a controller.

auto ensemble = ievolve::LLMEnsemble::Create(config->llm.models);
if (!ensemble.ok()) return;

auto evaluator = ievolve::evaluator::Evaluator::Create(config->evaluator, "evaluate.py");
if (!evaluator.ok()) return;

ievolve::controller::ControllerOptions settings;
settings.output_directory = "./run-output";
auto controller = ievolve::controller::Controller::Create(
    *config, std::move(*ensemble), std::move(*evaluator), settings);
if (!controller.ok()) return;

ievolve::controller::RunOptions run;
run.iterations = 100;
auto result = (*controller)->Run({seed_code}, run);
if (!result.ok()) return;
// result->best, result->stop_reason, result->checkpoint_path
```

Each `Controller` instance runs once; create a new one to resume from a checkpoint.
`InitialProgram` contains only code; the controller takes the language from `Config::language`,
which must be set before `Controller::Create`. The CLI handles inference and matching before
constructing the controller; library callers supply the resolved language themselves.
`PopulationStrategy` accepts callbacks for admission, cell replacement, archiving, eviction and
migration if you want to override the defaults.

---

## Limitations

- **Serial only.** The controller requires `database.in_memory: true` and runs one iteration at a
  time. Parallel workers are not implemented.
- **Explicitly unsupported**, returning `Unimplemented` rather than degrading silently: per-mutation
  disk mode, evolution tracing, worker recycling, embedding-based novelty (`embedding_model`,
  `embedding_api_base`), `memory_limit_mb`, `cpu_limit`, and `distributed`.
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
