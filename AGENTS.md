# Repository Guidelines

## Project Structure & Module Organization

This repository migrates OpenEvolve to C++17 one component at a time; behavioral parity with the Python reference in `openevolve/` is the primary correctness criterion. Eleven components under `ievolve/` (listed in `IEVOLVE_COMPONENTS` in the root `CMakeLists.txt`) are exported as `ievolve::<name>` static libraries, and `cli` builds the `build/ievolve/bin/ievolve` executable.

- `ievolve/utils/`: shared helpers (text, logging, threads, whole-file I/O in `file.h`); yaml-cpp is isolated in the separate `utils_yaml` target.
- `ievolve/config/`: shared types, JSON/YAML configuration, tests, and Python golden fixtures.
- `ievolve/program/`, `ievolve/code/`: the program record, metrics, and code parsing/distance.
- `ievolve/prompt/`: metrics, templates, artifacts, and sampling. `defaults/` holds templates embedded during CMake configuration (retain the upstream license); `utest_data/` holds Python reference outputs.
- `ievolve/process/`: centralized subprocess execution with timeouts and output caps (macOS/Linux).
- `ievolve/llm/`: unified interface, weighted ensemble, and Claude Code/Codex CLI clients, with `utest_data/` protocol fixtures. Tests use offline fake CLIs and never spend model quota.
- `ievolve/database/`: `ProgramDatabase` is a facade over `ProgramStore` (programs + ID index), `Population` (islands, MAP-Elites, archive, sampling, migration), and `database_codec` (checkpoint state). It owns the copy-and-swap transaction and must stay copyable; `checkpoint` and `artifact_store` handle on-disk persistence.
- `ievolve/evaluator/`, `ievolve/controller/`, `ievolve/cli/`: evaluation, the iteration runner and main loop, and CLI assembly with signal handling.
- `cmake/`: pinned dependencies and superbuild helpers. `CMakePresets.json` defines the `debug`, `release`, and `asan` presets.
- `tools/generate_*_fixtures.py`: Python reference fixture generators; config generation needs PyYAML and dacite.
- `openevolve/`: Python reference project; keep C++ migration work under `ievolve/`.
- `docs/`: local design + plan pairs (`docs/superpowers/{specs,plans}/`) written before each migration or larger refactor, plus validation records; ignored by Git and kept out of commits.

## Build, Test, and Development Commands

Run from the repository root. Install CMake 3.24+, a C++17 compiler, and a build tool. Initial configuration/build downloads pinned dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

These configure the superbuild, compile dependencies and components, and run tests. Use `ctest --test-dir build/ievolve --output-on-failure` to see individual component tests.

Presets provide equivalent workflows into `build/<preset>/`: `cmake --preset debug`, `cmake --build --preset debug`, and `ctest --preset debug`. Substitute `asan` for ASan + UBSan checks (its first build recompiles all dependencies); `release` builds the libraries and CLI without tests and has no test preset. With a preset, inner tests live under `build/<preset>/ievolve`, e.g. `ctest --test-dir build/asan/ievolve -L '^database$'`.

## Coding Style & Naming Conventions

Follow `.clang-format`: Google C++ style, two-space indentation, C++17, and a 120-column line limit. Separate logical blocks within function bodies with blank lines. Use snake_case filenames and variables, PascalCase types and ordinary functions, and trailing underscores for private members. Keep component files paired as `name.h`, `name.cc`, and `name_test.cc` in namespace `ievolve`, and don't implement one class across several `.cc` files. Register new units in the component's `CMakeLists.txt` source list and its `foreach(unit ...)` test list (`prompt/` uses `set(prompt_units ...)`).

An `if` branch may omit braces only when its condition and entire statement fit on one line. Use braces for multiline `if` and `else` branches, including branches with wrapped conditions.

```sh
clang-format --dry-run --Werror \
  ievolve/{utils,config,program,prompt,llm,code,database,process,evaluator,controller,cli}/*.h \
  ievolve/{utils,config,program,prompt,llm,code,database,process,evaluator,controller,cli}/*.cc
```

Return recoverable errors through `absl::Status` or `absl::StatusOr`; check `.ok()` before accessing values. Use `utils::ReadFile` / `utils::WriteFile` for whole-file I/O and map their status codes to the component's own errors at the call site. Configuration knobs that are not migrated yet must return `Unimplemented` rather than silently degrade.

## Testing Guidelines

Use GoogleTest with descriptive `TEST(ComponentTest, Behavior)` names. Cover changed behavior, error paths, and Python compatibility; no numeric coverage threshold is configured. Run CTest before submitting.

Regenerate fixtures with `python3 tools/generate_prompt_fixtures.py --source openevolve` and `python3 tools/generate_config_fixtures.py --source openevolve`; review output changes and document intentional compatibility differences in `README.md`.

## Commit & Pull Request Guidelines

Follow the existing imperative commit subjects, such as `Migrate prompt and config components to C++17` and `Rename testdata directories to utest_data`.

Keep PRs focused on one component or behavior. Describe the change, link relevant issues, report validation commands and results, and explain compatibility differences. Exclude build outputs, dependency caches, `openevolve/`, and `docs/`.
