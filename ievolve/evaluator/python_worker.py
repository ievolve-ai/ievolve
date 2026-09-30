"""Embedded Python evaluator worker (Python 3.9+, standard library only).

The saved stdout descriptor belongs to the protocol. Evaluator output, including
descriptor writes and inherited subprocess stdout, goes to captured stderr.
Process isolation provides lifecycle control, not a security sandbox.
"""

import base64
import importlib.util
import json
import math
import os
from pathlib import Path
import sys
import traceback
import types


class EvaluationResult:
    """Small compatibility surface for evaluator scripts using OpenEvolve."""

    def __init__(self, metrics, artifacts=None):
        self.metrics = metrics
        self.artifacts = {} if artifacts is None else artifacts

    @classmethod
    def from_dict(cls, metrics):
        return cls(metrics)

    def to_dict(self):
        return self.metrics

    def has_artifacts(self):
        return bool(self.artifacts)

    def get_artifact_keys(self):
        return list(self.artifacts)

    def get_artifact_size(self, key):
        if key not in self.artifacts:
            return 0
        value = self.artifacts[key]
        return len(value) if isinstance(value, bytes) else len(str(value).encode("utf-8"))

    def get_total_artifact_size(self):
        return sum(self.get_artifact_size(key) for key in self.artifacts)


class InvalidResult(Exception):
    pass


class ArtifactLimit(Exception):
    pass


def error(code, message):
    return {"kind": "error", "code": code, "message": message}


def exception_message(exception):
    try:
        return str(exception)
    except BaseException:
        return type(exception).__name__ + " (message unavailable)"


def validate_text(value):
    if not isinstance(value, str):
        raise InvalidResult("metric and artifact keys must be strings")
    value.encode("utf-8")


def encode_result(value, request):
    if isinstance(value, dict):
        metrics = value
        artifacts = {}
    else:
        try:
            metrics = value.metrics
            # Disabled artifacts are deliberately never inspected or serialized.
            artifacts = value.artifacts if request["enable_artifacts"] else {}
        except AttributeError:
            raise InvalidResult(
                "evaluate must return a metrics dict or an object with metrics and artifacts"
            ) from None
    if not isinstance(metrics, dict):
        raise InvalidResult("metrics must be a dict")
    for key, metric in metrics.items():
        validate_text(key)
        if metric is None or isinstance(metric, bool):
            continue
        if isinstance(metric, str):
            validate_text(metric)
        elif isinstance(metric, int):
            if metric < -(2**63) or metric > 2**64 - 1:
                raise InvalidResult("integer metric is outside the 64-bit JSON range")
        elif isinstance(metric, float):
            if not math.isfinite(metric):
                raise InvalidResult("metrics must be finite")
        else:
            raise InvalidResult("metrics must contain only JSON scalar values")
    # Python's float() would also accept numeric strings here; requiring a real
    # number keeps fitness parsing out of the C++ side.
    if "combined_score" in metrics:
        combined = metrics["combined_score"]
        if isinstance(combined, bool) or not isinstance(combined, (int, float)):
            raise InvalidResult("combined_score must be a number")
    if not isinstance(artifacts, dict):
        raise InvalidResult("artifacts must be a dict")
    encoded = {}
    total = 0
    for key, artifact in artifacts.items():
        validate_text(key)
        if isinstance(artifact, str):
            payload = artifact.encode("utf-8")
        elif isinstance(artifact, bytes):
            payload = artifact
        else:
            raise InvalidResult("artifacts must contain only text or bytes")
        total += len(payload)
        if total > request["max_artifact_bytes"]:
            raise ArtifactLimit("decoded artifact size limit exceeded")
        encoded[key] = (
            {"__bytes__": base64.b64encode(payload).decode("ascii")}
            if isinstance(artifact, bytes)
            else artifact
        )
    return {"metrics": metrics, "artifacts": encoded}


def exception_result(request):
    artifacts = {}
    if request["enable_artifacts"]:
        remaining = request["max_artifact_bytes"]
        for key, value in (("error", exception_message(sys.exc_info()[1])),
                           ("traceback", traceback.format_exc())):
            payload = value.encode("utf-8", errors="replace")[:remaining]
            text = payload.decode("utf-8", errors="ignore")
            if text:
                artifacts[key] = text
            remaining -= len(text.encode("utf-8"))
    return {"kind": "result", "failed": True,
            "result": {"metrics": {"error": 0}, "artifacts": artifacts}}


def run(request):
    if sys.version_info < (3, 9):
        return error("failed_precondition", "Python 3.9 or newer is required")
    package = types.ModuleType("openevolve")
    package.__path__ = []
    compatibility = types.ModuleType("openevolve.evaluation_result")
    compatibility.EvaluationResult = EvaluationResult
    package.evaluation_result = compatibility
    sys.modules["openevolve"] = package
    sys.modules["openevolve.evaluation_result"] = compatibility
    evaluation_file = Path(request["evaluation_file"])
    sys.path.insert(0, str(evaluation_file.parent))
    try:
        spec = importlib.util.spec_from_file_location("_ievolve_evaluator", evaluation_file)
        if spec is None or spec.loader is None:
            raise ImportError("cannot load evaluator module")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        if not callable(getattr(module, "evaluate", None)):
            raise TypeError("evaluator must define a callable evaluate")
        if request["action"] == "inspect":
            stages = []
            for stage in range(1, 4):
                function = getattr(module, "evaluate_stage" + str(stage), None)
                if function is not None:
                    if not callable(function):
                        raise TypeError("evaluate_stage" + str(stage) + " must be callable")
                    stages.append(stage)
            return {"kind": "inspect", "stages": stages}
        stage = request["stage"]
        name = "evaluate" if stage == 0 else "evaluate_stage" + str(stage)
        function = getattr(module, name, None)
        if not callable(function):
            raise TypeError("evaluator must define a callable " + name)
    except BaseException:
        return error("failed_precondition", traceback.format_exc())
    try:
        value = function(request["candidate_file"])
    except BaseException:
        return exception_result(request)
    try:
        result = encode_result(value, request)
        # Check encoding here so malformed Unicode never escapes as a worker crash.
        json.dumps(result, allow_nan=False, ensure_ascii=False).encode("utf-8")
        return {"kind": "result", "failed": False, "result": result}
    except ArtifactLimit as exception:
        return error("resource_exhausted", exception_message(exception))
    except Exception as exception:
        return error("data_loss", exception_message(exception))


def main():
    protocol = os.dup(1)
    os.set_inheritable(protocol, False)
    os.dup2(2, 1)
    try:
        request = json.load(sys.stdin)
        response = run(request)
        payload = json.dumps(response, allow_nan=False, ensure_ascii=True).encode("utf-8")
        while payload:
            written = os.write(protocol, payload)
            payload = payload[written:]
    finally:
        os.close(protocol)


if __name__ == "__main__":
    main()
