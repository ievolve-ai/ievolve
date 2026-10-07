# Configuration reference fixtures

Regenerate `python_golden.json` with
`python3 tools/generate_config_fixtures.py --source openevolve`.
The file preserves upstream inputs and outputs. Tests compare unchanged
algorithm/configuration behavior after removing intentionally unsupported fields.

CLI command configuration (`run`, `candidate`, `evaluator`) is validated by
native tests. Removed Python evaluator retry/cascade/resource/parallel settings
remain visible in the raw reference data but are rejected by the new schema;
they are never loaded as a production compatibility path.
