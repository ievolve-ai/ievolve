"""Offline fixture: sibling imports, descriptor output, and fresh module state."""

import os
from pathlib import Path
import runpy
import subprocess
import sys

from openevolve.evaluation_result import EvaluationResult
from python_support import VALUE

print("import output")
os.write(1, b"descriptor output\n")
subprocess.run([sys.executable, "-c", "print('child output')"], check=True)
calls = 0


def evaluate(candidate):
    global calls
    calls += 1
    answer = runpy.run_path(candidate)["answer"]()
    return EvaluationResult(
        metrics={"combined_score": answer, "calls": calls, "ok": answer == VALUE},
        artifacts={"text": "\u03bb", "binary": b"\x00\xff\x80", "cwd": str(Path.cwd())},
    )


def evaluate_stage1(candidate):
    return evaluate(candidate)


def evaluate_stage3(candidate):
    return EvaluationResult.from_dict({"combined_score": 3})
