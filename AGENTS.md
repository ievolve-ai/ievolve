# Repository Guidelines

## Project Structure & Module Organization

This repository migrates OpenEvolve to C++17 one component at a time. Components under `ievolve/` expose `ievolve::prompt`, `ievolve::config`, and `ievolve::llm`; there is no standalone C++ application yet.

- `ievolve/prompt/`: headers, implementations, and colocated `*_test.cc` tests for metrics, templates, artifacts, and sampling.
- `ievolve/config/`: shared types, JSON/YAML configuration, tests, and Python golden fixtures.
- `ievolve/llm/`: unified interface, weighted ensemble, Claude Code/Codex CLI clients, process runner, and `utest_data/` protocol fixtures. Tests use offline fake CLIs.
- `ievolve/prompt/defaults/`: templates embedded during CMake configuration; retain the upstream license.
- `ievolve/prompt/utest_data/`: golden fixtures containing Python reference outputs.
- `cmake/`: pinned dependencies and superbuild helpers.
- `tools/generate_*_fixtures.py`: Python reference fixture generators; config generation needs PyYAML and dacite.
- `openevolve/`: Python reference project; keep C++ migration work under `ievolve/`.
- `docs/`: local migration designs, plans, and validation records; ignored by Git and kept out of commits.

## Build, Test, and Development Commands

Run from the repository root. Install CMake 3.24+, a C++17 compiler, and a build tool. Initial configuration/build downloads pinned dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

These configure the superbuild, compile dependencies and components, and run tests. Use `ctest --test-dir build/ievolve --output-on-failure` to see individual component tests.

Presets provide equivalent workflows: `cmake --preset debug`, `cmake --build --preset debug`, and `ctest --preset debug`. Substitute `asan` for sanitizer checks; `release` configures a library-only build without tests.

## Coding Style & Naming Conventions

Follow `.clang-format`: Google C++ style, two-space indentation, C++17, and a 120-column line limit. Separate logical blocks within function bodies with blank lines. Use snake_case filenames and variables, PascalCase types and ordinary functions, and trailing underscores for private members. Keep component files paired as `name.h`, `name.cc`, and `name_test.cc` in namespace `ievolve`.

An `if` branch may omit braces only when its condition and entire statement fit on one line. Use braces for multiline `if` and `else` branches, including branches with wrapped conditions.

```sh
clang-format --dry-run --Werror \
  ievolve/{utils,config,program,prompt,llm,code,database,process,evaluator,controller,cli}/*.h \
  ievolve/{utils,config,program,prompt,llm,code,database,process,evaluator,controller,cli}/*.cc
```

Return recoverable errors through `absl::Status` or `absl::StatusOr`; check `.ok()` before accessing values.

## Testing Guidelines

Use GoogleTest with descriptive `TEST(ComponentTest, Behavior)` names. Cover changed behavior, error paths, and Python compatibility; no numeric coverage threshold is configured. Run CTest before submitting.

Regenerate fixtures with `python3 tools/generate_prompt_fixtures.py --source openevolve` and `python3 tools/generate_config_fixtures.py --source openevolve`; review output changes and document intentional compatibility differences in `README.md`.

## Commit & Pull Request Guidelines

Follow the existing imperative commit subjects, such as `Migrate prompt and config components to C++17` and `Rename testdata directories to utest_data`.

Keep PRs focused on one component or behavior. Describe the change, link relevant issues, report validation commands and results, and explain compatibility differences. Exclude build outputs and dependency caches.
