# Program reference fixtures

Regenerate from the repository root with
`python3 tools/generate_database_fixtures.py --source openevolve`.
The generator executes the unchanged Program class extracted with Python AST,
without importing unrelated LLM/embedding dependencies. Nine complete JSON
cases cover defaults, legacy descriptions, optionals, metadata and unknown fields.
Timestamps are fixed in fixtures; a separate test checks the live default clock.
