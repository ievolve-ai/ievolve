# CLI protocol fixtures

These are hand-authored offline examples, not recordings from paid model calls.

- `claude_success.json`: result envelope, final text, token counts and cost from
  the [Claude print-mode JSON contract](https://code.claude.com/docs/en/headless).
- `codex_success.jsonl`: reasoning, commentary, final answer and terminal usage
  following the [Codex JSONL event contract](https://learn.chatgpt.com/docs/non-interactive-mode).

The client tests also construct malformed and failed envelopes in memory.
Programs in `../utest_helpers/` exercise the adapters and process runner through
real local subprocesses. Keep sample credentials and provider account identifiers
out of these files.
