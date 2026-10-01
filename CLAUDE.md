# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

`ievolve` is a component-by-component C++17 port of **OpenEvolve** (an LLM-driven evolutionary
code-optimization loop). The Python original is vendored, untracked, at `openevolve/` and serves
as the **reference implementation**; the C++ port lives under `ievolve/`. Every migrated component
is validated against golden fixtures generated from that Python source, so behavioral parity with
`openevolve/` is the primary correctness criterion — not "what looks right in C++".

`README.md` (Chinese) is the authoritative record of migrated behavior and documented
compatibility divergences. `AGENTS.md` holds the same conventions in condensed English form.
`docs/` is **local-only and gitignored**: it holds the design + plan pair written before each
migration or larger refactor (`docs/superpowers/{specs,plans}/`) and validation notes. Write new
design docs there, but never commit them.

## Build and test

Requires CMake 3.24+, a C++17 compiler, Python 3.9+ (evaluator/CLI tests). Run from the repo root.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

`CMakePresets.json` defines the same workflow into `build/<preset>/`:
`cmake --preset debug && cmake --build --preset debug -j 8 && ctest --preset debug`.
`asan` adds ASan + UBSan; `release` builds libraries and the CLI with `BUILD_TESTING=OFF`, so it has
no test preset. With presets every `build/...` path below becomes `build/<preset>/...`, e.g. the
inner tests are `ctest --test-dir build/debug/ievolve`.

### The two-layer build (important)

The root `CMakeLists.txt` is a **superbuild**. With `IEVOLVE_SUPERBUILD=ON` (default) it builds the
pinned third-party deps via `ExternalProject_Add`, then re-invokes itself with
`IEVOLVE_SUPERBUILD=OFF` into `build/ievolve/`. Consequences:

- Outer `ctest --test-dir build` runs only one forwarding test per component, named
  `ievolve.<component>`.
- Inner `ctest --test-dir build/ievolve` runs the actual GoogleTest cases — use this while iterating.
- Test binaries and the CLI land under `build/ievolve/ievolve/<component>/` and `build/ievolve/bin/`.
- `build/ievolve/compile_commands.json` is the one clangd should use.

### Running a subset

```sh
# Outer forwarder for one component (utils config program prompt process llm
# code database evaluator controller cli).
ctest --test-dir build -L '^llm$' --output-on-failure
# Inner project: all unit tests, or one component's unit tests.
ctest --test-dir build/ievolve --output-on-failure
ctest --test-dir build/ievolve -L '^llm$' --output-on-failure
# A single test binary / single case.
./build/ievolve/ievolve/prompt/prompt_sampler_test --gtest_filter='PromptSamplerTest.*'
ctest --test-dir build/ievolve -R 'PromptSamplerTest' --output-on-failure
```

Some tests need environment/definitions that CTest supplies (`IEVOLVE_TEST_DATA_DIR`,
`IEVOLVE_CONFIG_TEST_KEY=fixture-key`, `ENABLE_ARTIFACTS=true`, `IEVOLVE_PYTHON_EXECUTABLE`), so
prefer `ctest -R` over running binaries bare when a test reads fixtures or env.

### Library-only / offline / sanitizers

```sh
# No tests, no GoogleTest download.
cmake -S . -B build-release -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
# Offline: dir must hold abseil.tar.gz json.tar.gz yaml_cpp.tar.gz googletest.tar.gz
# matching the SHA256s in cmake/Dependencies.cmake.
cmake -S . -B build -DIEVOLVE_DEPENDENCY_ARCHIVE_DIR=/path/to/archives
# ASan + UBSan (project sources only; deps stay unsanitized). The first configure
# rebuilds every dependency into build/asan, which takes several minutes.
cmake --preset asan && cmake --build --preset asan -j 8 && ctest --preset asan
ctest --test-dir build/asan/ievolve -L '^database$' --output-on-failure
```

Reusing an existing build dir requires re-passing any custom `IEVOLVE_*` cache variables.

### Format check

```sh
clang-format --dry-run --Werror \
  ievolve/{utils,config,program,prompt,llm,code,database,process,evaluator,controller,cli}/*.h \
  ievolve/{utils,config,program,prompt,llm,code,database,process,evaluator,controller,cli}/*.cc
```

### Regenerating Python golden fixtures

Fixtures in `ievolve/<component>/utest_data/` are produced from the vendored Python reference.
Regenerate, then diff and justify any change — a fixture diff means a behavior divergence.

```sh
python3 tools/generate_prompt_fixtures.py --source openevolve   # one per component
```

`generate_config_fixtures.py` needs PyYAML + dacite; `generate_evaluator_fixtures.py` needs
Python 3.12+ (the evaluator worker itself still only needs 3.9+).

### Running the CLI end to end

```sh
python3 -m pip install -r examples/circle_packing/requirements.txt
# Set max_iterations: 0 and run.python_executable in this YAML first.
build/ievolve/bin/ievolve --config examples/circle_packing/config.yaml
```

`max_iterations: 0` in YAML evaluates the seed program without invoking any model — use it to exercise the
pipeline without spending model quota (keep evaluator.use_llm_feedback false). Runtime settings
come only from YAML; the CLI accepts --config, --help and --version. Nonzero iterations drive the logged-in `codex` or
`claude_code` CLI for real. Exit codes: 0 done, 1 run failure, 2 argument/config error,
130/143 on SIGINT/SIGTERM (progress is checkpointed first).

## Architecture

Eleven components under `ievolve/`, each registered by `ievolve/CMakeLists.txt` from the
`IEVOLVE_COMPONENTS` list in the root `CMakeLists.txt`. Exported as `ievolve::<name>` aliases over
`ievolve_<name>` targets. Every component links `utils` (text, logging, threads, whole-file
I/O in `utils/file.h`); yaml-cpp is isolated in the separate `utils_yaml` target. Dependencies flow
strictly one way:

```
config_types (header-only)  →  program → prompt
config (YAML/JSON)          →  code  → database
process → llm, evaluator
                       controller (iteration + main loop)  →  cli → bin/ievolve
```

Layering rules that the code actually enforces and that you should preserve:

- `prompt` links `config_types` only — it must not pull in yaml-cpp. `config_types` is an INTERFACE
  target precisely so `prompt` stays free of the YAML parser.
- `database` depends on program/code/config types only — never on prompt, llm, YAML, or Python.
- `controller/iteration.cc` (`IterationRunner`) owns one attempt: prompt → LLM → code edit →
  evaluate, and returns a candidate. It deliberately holds **no mutable database**. Parent
  selection, child-ID allocation, insertion, prompt logging, checkpointing and stop conditions all
  live in `controller/controller.cc` (`Controller`, one `Run` per instance).
- `cli` assembles config/LLM/evaluator/controller and owns signal handling (`cli/stop.h`).

Component-internal notes worth knowing before editing:

- **Resources are embedded at configure time.** `ievolve/prompt/defaults/*.txt` + `fragments.json`
  are read by `prompt/CMakeLists.txt` into `embedded_templates.inc`; `evaluator/python_worker.py`
  is embedded via `python_worker.inc.in`. The runtime needs neither Python nor the template dir.
  Editing those data files requires a CMake re-configure (a `CONFIGURE_DEPENDS` glob triggers it).
- **LLM access is CLI-only.** `ievolve::llm` shells out to the installed `claude_code` / `codex`
  binaries using their existing login; there is no HTTP client and `api_key` in config is unused
  for generation. Providers are exactly `claude_code` and `codex` (the old `codex_cli` name is
  gone). `llm/no_generation.h` provides the zero-iteration stub. Tests use fake local processes and
  fixtures in `llm/utest_data/` — they never spend quota, and new tests must not either.
- **Subprocess execution is centralized** in `ievolve::process` (`RunProcess`, timeouts, process-group
  kill, 8 MiB combined output cap). macOS/Linux only; Windows returns `Unimplemented`.
  `ievolve/llm/process.h` is a back-compat forwarding header.
- **`ProgramDatabase` is a facade over three units.** `ProgramStore` (`program_store.*`) holds
  programs in insertion order with an ID index; `Population` (`population.*`) runs islands,
  MAP-Elites cells, the archive, sampling and migration, taking the store as a parameter and never
  keeping a pointer to it; `database_codec.*` maps both to and from `CheckpointData`.
  `ProgramDatabase` owns the copy-and-swap transaction (`Mutate` copies a `State` of store +
  population), nested-mutation rejection, disk persistence and artifacts. It must stay copyable:
  `Controller::Step` copies the whole database to roll back an attempt. Sampling consumes the
  persisted RNG, so it runs through `Mutate`; never reorder RNG calls.
- **Persistence** lives in `database/checkpoint.*` and `database/artifact_store.*`: a native format
  with a `CURRENT` pointer to `snapshots/<generation>/`, plus an importer for Python's
  `metadata.json + programs/*.json`. Writes publish atomically; a failed write leaves memory state
  untouched. Use `utils::ReadFile` / `utils::WriteFile` for whole-file I/O and map their status
  codes to the component's own errors at the call site.
- **Not yet migrated**, and returning `Unimplemented` rather than silently degrading: parallel
  workers, per-mutation disk mode, trace export, worker recycling, embedding-based novelty
  (`embedding_model` / `embedding_api_base`), `memory_limit_mb` / `cpu_limit` / `distributed`.
  Keep that pattern — an unmigrated knob errors loudly.

## Conventions

- Google C++ style via `.clang-format`, two-space indent, C++17. `snake_case` files and variables,
  `PascalCase` types and functions, trailing underscore on private members. Namespace `ievolve`
  (`ievolve::controller`, `ievolve::evaluator` for those two).
- Each unit is the triple `name.h` / `name.cc` / `name_test.cc`, colocated, with its own test
  executable declared in the component's `CMakeLists.txt`. Most components register tests from a
  `foreach(unit ...)` list (`prompt/` uses `set(prompt_units ...)`); add new units there and to
  the library's source list. Don't implement one class across several `.cc` files.
- Recoverable errors return `absl::Status` / `absl::StatusOr`; check `.ok()` before dereferencing.
  Diagnostics carry field locations, never config values, prompts, or raw model output.
- Tests are GoogleTest with `TEST(ComponentTest, Behavior)` names; every test is registered with
  `gtest_discover_tests(... PROPERTIES LABELS "<component>")`, which is what the `-L` filters use.
- New behavior should be covered by a fixture compared against `openevolve/` when the behavior
  exists upstream; intentional divergences get documented in `README.md` (see the
  "迁移行为与边界" section) rather than silently introduced.
- Commit subjects are imperative and component-scoped, e.g. `Migrate iteration runner and serial
  controller to C++17`. Keep changes scoped to one component. Never commit `build*/` output,
  `openevolve/` or `docs/` (all gitignored).
