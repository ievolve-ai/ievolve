# Database reference fixtures

Regenerate with `python3 tools/generate_database_fixtures.py --source openevolve`.
Seven scenarios execute the unchanged Python global get/best/top methods on
manually populated records. Each uses multiple metric choices and Top-N limits.
Python AST extraction avoids optional dependencies and constructor side effects.

These fixtures verify read-only query semantics, not Python's island admission,
population strategies or persistence. C++ ownership, duplicate-ID rejection,
finite score validation and numeric-only named metrics have separate tests.

`features.json` has 11 feature-coordinate sequences. Regenerate with
`python3 tools/generate_feature_fixtures.py --source openevolve`. It executes
unchanged feature precedence, complexity/score and minmax-scaling methods.
Multi-program builtin diversity is excluded: C++ deliberately uses deterministic
Unicode edit distance instead of Python's cached heuristic/reference selection.

`population.json` has seven sequences / 32 additions. Regenerate with
`python3 tools/generate_population_fixtures.py --source openevolve`. The generator
executes unchanged add, cell replacement, archive, island ownership, capacity
and best-program methods against manually initialized in-memory state. It sets
`embedding_client=None` and `db_path=None`; there are no method stubs. Inputs
have explicit iterations and unique fitness to avoid Python set-order ties.
The fixture records the reference commit and source hash.

Population fixtures exclude random sampling, migration, hooks, builtin
diversity, zero archive capacity, implicit iteration updates and Python's
already-archived replacement eviction quirk. Those supported C++ behaviors
have separate unit/invariant tests; intentional differences are in README.md.

`artifacts.json` has seven Python cases for byte sizing, inline JSON/base64 and
flat-directory text/binary loading. Regenerate with
`python3 tools/generate_artifact_fixtures.py --source openevolve`. Native typed
manifests, replacement behavior and retention are covered by C++ filesystem tests.

`checkpoint_legacy/` is a two-program/two-island checkpoint written by unchanged
Python save helpers using fixed state. Regenerate with
`python3 tools/generate_checkpoint_fixtures.py --source openevolve`. It includes
feature history, counters, prompt logs, inline binary and a relative disk
artifact; reference.json records its scope and configuration. C++ tests import,
convert, relocate and continue from it. Native atomic generation publication and
RNG restoration are C++ contracts, not claims of Python byte-for-byte output.
