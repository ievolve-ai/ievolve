#!/usr/bin/env python3
"""Show the prompts ievolve stored for accepted programs.

The controller records each accepted child's system prompt, user prompt, model
response and token usage in the database checkpoint (database.log_prompts,
on by default). Rejected or failed iterations are not recorded.

Usage:
  python3 show_prompts.py OUTPUT_DIR                  list every program with a prompt
  python3 show_prompts.py OUTPUT_DIR iteration-66     print that program's prompt
  python3 show_prompts.py OUTPUT_DIR 66 --part user   only the user prompt

PATH may be a run output directory (holding checkpoints/), one checkpoint
directory, or a checkpoint's database/ directory. A checkpoint keeps only the
programs still in the population, so every checkpoint is searched and the
newest copy of each program wins.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

PARTS = ("system", "user", "response")


def database_dirs(path):
    """Return database directories under path, newest checkpoint first."""
    if (path / "checkpoints").is_dir():
        checkpoints = [p for p in (path / "checkpoints").iterdir() if re.fullmatch(r"checkpoint_\d+", p.name)]
        checkpoints.sort(key=lambda p: int(p.name.split("_")[1]), reverse=True)
        return [p / "database" for p in checkpoints if (p / "database" / "CURRENT").is_file()]
    if (path / "database" / "CURRENT").is_file():
        return [path / "database"]
    if (path / "CURRENT").is_file():
        return [path]
    raise SystemExit(f"No ievolve checkpoint found under {path}")


def load_programs(database):
    """Yield every program stored in a database checkpoint's current snapshot."""
    snapshot = database / "snapshots" / (database / "CURRENT").read_text().strip()
    for file in sorted((snapshot / "programs").glob("*.json")):
        with file.open(encoding="utf-8") as stream:
            yield json.load(stream)


def collect(path):
    """Map program id to its newest stored copy that carries a prompt."""
    programs = {}
    for database in database_dirs(path):
        for program in load_programs(database):
            if program.get("prompts") and program["id"] not in programs:
                programs[program["id"]] = program
    return programs


def prompt_entry(program):
    """Return (template_key, entry) for the program's recorded prompt."""
    key, entry = next(iter(program["prompts"].items()))
    return key, entry


def list_programs(programs):
    header = f"{'id':<16}{'iter':>6}{'score':>18}  {'template':<18}{'system':>8}{'user':>9}{'reply':>8}{'in_tok':>8}{'out_tok':>8}"
    print(header)
    print("-" * len(header))
    for program in sorted(programs.values(), key=lambda p: p.get("iteration_found", 0)):
        key, entry = prompt_entry(program)
        usage = entry.get("token_usage") or {}
        reply = sum(len(r) for r in entry.get("responses", []))
        score = program.get("metrics", {}).get("combined_score")
        score_text = f"{score:.13f}" if isinstance(score, (int, float)) else "-"
        print(f"{program['id']:<16}{program.get('iteration_found', 0):>6}{score_text:>18}  {key:<18}"
              f"{len(entry.get('system', '')):>8}{len(entry.get('user', '')):>9}{reply:>8}"
              f"{usage.get('input_tokens', '-'):>8}{usage.get('output_tokens', '-'):>8}")


def show_program(program, parts):
    key, entry = prompt_entry(program)
    usage = entry.get("token_usage")
    print(f"# {program['id']} (iteration {program.get('iteration_found')}, template {key}, usage {usage})")
    texts = {
        "system": entry.get("system", ""),
        "user": entry.get("user", ""),
        "response": "\n\n--- next response ---\n\n".join(entry.get("responses", [])),
    }
    for part in parts:
        print(f"\n===== {part.upper()} =====\n")
        print(texts[part])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", type=Path, help="run output directory, checkpoint directory or database directory")
    parser.add_argument("program", nargs="?", help="program id such as iteration-66, or just the iteration number")
    parser.add_argument("--part", choices=PARTS + ("all",), default="all", help="which part to print (default: all)")
    args = parser.parse_args()

    programs = collect(args.path)
    if not programs:
        raise SystemExit("No stored prompts found (is database.log_prompts disabled?)")

    if args.program is None:
        list_programs(programs)
        return

    program_id = f"iteration-{args.program}" if args.program.isdigit() else args.program
    program = programs.get(program_id)
    if program is None:
        raise SystemExit(f"No stored prompt for {program_id}; run without a program id to list them")
    show_program(program, PARTS if args.part == "all" else (args.part,))


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:  # e.g. piped into head
        # Point stdout at devnull so the interpreter's final flush stays quiet.
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        sys.exit(0)
