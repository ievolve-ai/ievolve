# Python reference fixtures

`python_golden.json` is generated from the unchanged
`openevolve/openevolve/utils/code_utils.py`; its reference commit is recorded in
the JSON file. Regenerate from the repository root:

```sh
python3 tools/generate_code_fixtures.py --source openevolve
```

Only the Python standard library is required. The 90 cases compare complete
outputs or expected error status for diff parsing/application/routing, full
rewrites, editable regions, and summaries. C++ tests also cover documented
differences: literal language names and rejected invalid summary limits.
