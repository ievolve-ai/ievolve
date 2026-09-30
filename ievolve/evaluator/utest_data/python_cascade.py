"""Offline complete cascade for C++ orchestration integration tests."""

import runpy
from openevolve.evaluation_result import EvaluationResult


def evaluate(path):
    return {"combined_score": runpy.run_path(path)["score"]}


def evaluate_stage1(path):
    return EvaluationResult({"combined_score": 0.5}, {"candidate_path": path})


def evaluate_stage2(path):
    candidate = runpy.run_path(path)
    if "stage2_hook" in candidate:
        candidate["stage2_hook"]()
    return EvaluationResult({"combined_score": 0.75}, {"binary": b"\x00\xff"})


def evaluate_stage3(path):
    return {"combined_score": 0.9}
