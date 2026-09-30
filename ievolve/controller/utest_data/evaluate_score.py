"""Offline evaluation script for the single-iteration integration test."""

import runpy
from pathlib import Path
from openevolve.evaluation_result import EvaluationResult


def evaluate(path):
    return EvaluationResult(
        metrics={"combined_score": runpy.run_path(path)["score"]},
        artifacts={"source": Path(path).read_text(), "binary": b"\x00\xff"},
    )
