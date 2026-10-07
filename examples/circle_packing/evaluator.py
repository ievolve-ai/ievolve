"""Validate 26 circles without an OpenEvolve runtime dependency.

The protocol v1 evaluator starts the candidate CLI with bounded output and a
deadline. The framework owns the outer process group. Geometry uses tolerance
1e-12 in unit-square coordinates: a few orders above float64 rounding in the
slack arithmetic (~1e-16), so tangent circles are not rejected, yet too small
for inflated radii to buy a meaningful score. Negative radii are never tolerated.
"""

import json
import math
import os
from pathlib import Path
import selectors
import subprocess
import sys
import time
from types import SimpleNamespace

import numpy as np


CIRCLE_COUNT = 26
TOLERANCE = 1e-12
REFERENCE_SUM = 2.635


def _json_number(value):
    """Represent unavailable/non-finite diagnostics as JSON null, never NaN."""
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return None
    return number if math.isfinite(number) else None


def _json_array(values):
    if values.ndim == 0:
        return _json_number(values.item())
    return [_json_array(value) for value in values]


def _result(metrics, diagnostics, packing=None):
    artifacts = {
        "validation.json": json.dumps(diagnostics, allow_nan=False, sort_keys=True),
    }
    if packing is not None:
        artifacts["packing.json"] = json.dumps(
            packing, allow_nan=False, sort_keys=True
        )
    return SimpleNamespace(metrics=metrics, artifacts=artifacts)


def assess_packing(centers, radii, reported_sum=None):
    """Score real numeric arrays; preserve constraint residuals as artifacts.

    A positive residual means clearance; a negative residual means violation.
    Pair residuals follow (0,1), (0,2), ..., (24,25). Boundary residuals follow
    left, bottom, right, top for each circle. Invalid packings score zero.
    The candidate's reported sum is diagnostic only, even if it is non-finite.
    """
    metrics = {
        "validity": 0.0,
        "sum_radii": 0.0,
        "target_ratio": 0.0,
        "combined_score": 0.0,
    }
    diagnostics = {
        "errors": [],
        "circle_count_required": CIRCLE_COUNT,
        "tolerance": TOLERANCE,
        "reference_sum": REFERENCE_SUM,
        "reported_sum": _json_number(reported_sum),
    }
    errors = diagnostics["errors"]
    try:
        centers = np.asarray(centers)
        radii = np.asarray(radii)
    except (TypeError, ValueError) as error:
        errors.append(f"Cannot convert centers/radii to arrays: {error}")
        return _result(metrics, diagnostics)
    diagnostics["centers_shape"] = list(centers.shape)
    diagnostics["radii_shape"] = list(radii.shape)
    if centers.shape != (CIRCLE_COUNT, 2) or radii.shape != (CIRCLE_COUNT,):
        errors.append("Expected centers shape (26, 2) and radii shape (26,)")
        return _result(metrics, diagnostics)
    if centers.dtype.kind not in "biuf" or radii.dtype.kind not in "biuf":
        errors.append("Centers and radii must contain real numeric values")
        return _result(metrics, diagnostics)
    with np.errstate(over="ignore", invalid="ignore"):
        centers = centers.astype(float)
        radii = radii.astype(float)
    packing = {
        "centers": _json_array(centers),
        "radii": _json_array(radii),
        "reported_sum": diagnostics["reported_sum"],
    }
    if not np.isfinite(centers).all() or not np.isfinite(radii).all():
        errors.append("Centers and radii must be finite (no NaN or infinity)")
        return _result(metrics, diagnostics, packing)

    negative_radii = np.flatnonzero(radii < 0).tolist()
    if negative_radii:
        errors.append(f"Negative radii at indices {negative_radii}")
    with np.errstate(over="ignore", invalid="ignore"):
        boundary_slacks = np.column_stack(
            (centers[:, 0] - radii, centers[:, 1] - radii,
             1.0 - centers[:, 0] - radii, 1.0 - centers[:, 1] - radii)
        )
        first, second = np.triu_indices(CIRCLE_COUNT, k=1)
        deltas = centers[first] - centers[second]
        pair_slacks = np.hypot(deltas[:, 0], deltas[:, 1]) - (
            radii[first] + radii[second]
        )
    try:
        actual_sum = _json_number(math.fsum(radii.tolist()))
    except OverflowError:
        actual_sum = None
    packing["actual_sum_radii"] = actual_sum
    diagnostics["actual_sum_radii"] = actual_sum
    diagnostics["negative_radius_count"] = len(negative_radii)
    diagnostics["boundary_slacks"] = _json_array(boundary_slacks)
    diagnostics["pair_slacks"] = _json_array(pair_slacks)
    diagnostics["boundary_order"] = ["left", "bottom", "right", "top"]
    diagnostics["pair_order"] = "i=0..24, j=i+1..25"

    if (actual_sum is None or not np.isfinite(boundary_slacks).all()
            or not np.isfinite(pair_slacks).all()):
        errors.append("Geometry or sum overflowed finite floating-point arithmetic")
        return _result(metrics, diagnostics, packing)

    min_boundary = float(np.min(boundary_slacks))
    min_pair = float(np.min(pair_slacks))
    boundary_index = np.unravel_index(np.argmin(boundary_slacks), boundary_slacks.shape)
    pair_index = int(np.argmin(pair_slacks))
    outside_count = int(np.count_nonzero(np.any(boundary_slacks < -TOLERANCE, axis=1)))
    overlap_count = int(np.count_nonzero(pair_slacks < -TOLERANCE))
    diagnostics.update({
        "min_boundary_slack": min_boundary,
        "min_pair_slack": min_pair,
        "worst_boundary_circle": int(boundary_index[0]),
        "worst_boundary_side": diagnostics["boundary_order"][boundary_index[1]],
        "worst_pair": [int(first[pair_index]), int(second[pair_index])],
        "outside_circle_count": outside_count,
        "overlap_pair_count": overlap_count,
    })
    metrics.update({
        "actual_sum_radii": actual_sum,
        "min_boundary_slack": min_boundary,
        "min_pair_slack": min_pair,
        "max_boundary_violation": max(0.0, -min_boundary),
        "max_overlap": max(0.0, -min_pair),
        "outside_circle_count": outside_count,
        "overlap_pair_count": overlap_count,
        "negative_radius_count": len(negative_radii),
    })
    if diagnostics["reported_sum"] is not None:
        diagnostics["reported_sum_error"] = _json_number(
            diagnostics["reported_sum"] - actual_sum
        )
    if outside_count:
        errors.append(f"{outside_count} circles exceed the square by more than {TOLERANCE}")
    if overlap_count:
        errors.append(f"{overlap_count} circle pairs overlap by more than {TOLERANCE}")
    if not errors:
        metrics.update({
            "validity": 1.0,
            "sum_radii": actual_sum,
            "target_ratio": actual_sum / REFERENCE_SUM,
            "combined_score": actual_sum,
        })
    return _result(metrics, diagnostics, packing)



OUTPUT_LIMIT = 1024 * 1024


class Rejected(Exception):
    pass


def run_candidate(argv, cwd, payload, timeout):
    """Bound both streams while running; inherit the framework's process group."""
    start = time.monotonic()
    process = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output = bytearray()
    total = 0
    offset = 0
    try:
        with selectors.DefaultSelector() as selector:
            for stream in (process.stdin, process.stdout, process.stderr):
                os.set_blocking(stream.fileno(), False)
            if payload:
                selector.register(process.stdin, selectors.EVENT_WRITE)
            else:
                process.stdin.close()
            selector.register(process.stdout, selectors.EVENT_READ)
            selector.register(process.stderr, selectors.EVENT_READ)
            while selector.get_map() or process.poll() is None:
                remaining = timeout - (time.monotonic() - start)
                if remaining <= 0:
                    raise Rejected("candidate_timeout")
                for key, _ in selector.select(min(remaining, 0.05)):
                    stream = key.fileobj
                    if stream is process.stdin:
                        try:
                            offset += os.write(stream.fileno(), payload[offset:offset + 4096])
                        except BrokenPipeError:
                            offset = len(payload)
                        except BlockingIOError:
                            continue
                        if offset == len(payload):
                            selector.unregister(stream)
                            stream.close()
                    else:
                        try:
                            chunk = os.read(stream.fileno(), 4096)
                        except BlockingIOError:
                            continue
                        if not chunk:
                            selector.unregister(stream)
                            stream.close()
                            continue
                        total += len(chunk)
                        if total > OUTPUT_LIMIT:
                            raise Rejected("candidate_output_limit")
                        if stream is process.stdout:
                            output.extend(chunk)
            return process.returncode, bytes(output), (time.monotonic() - start) * 1000
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        for stream in (process.stdin, process.stdout, process.stderr):
            stream.close()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate request key")
        result[key] = value
    return result


def validate_request(request):
    if (not isinstance(request, dict)
            or set(request) != {"protocol_version", "candidate", "limits", "enable_artifacts"}
            or type(request["protocol_version"]) is not int or request["protocol_version"] != 1
            or type(request["enable_artifacts"]) is not bool):
        raise ValueError("invalid protocol v1 request")
    candidate = request["candidate"]
    if not isinstance(candidate, dict) or set(candidate) != {"argv", "cwd"}:
        raise ValueError("invalid candidate description")
    argv = candidate["argv"]
    cwd = candidate["cwd"]
    if (not isinstance(argv, list) or not argv or not argv[0]
            or any(not isinstance(arg, str) or "\0" in arg for arg in argv)
            or not isinstance(cwd, str) or "\0" in cwd or not Path(cwd).is_absolute()
            or not Path(cwd).is_dir()):
        raise ValueError("invalid candidate command")
    limits = request["limits"]
    if not isinstance(limits, dict) or set(limits) != {"evaluation_timeout_ms", "max_artifact_bytes"}:
        raise ValueError("invalid limits")
    if (type(limits["evaluation_timeout_ms"]) is not int or not 0 < limits["evaluation_timeout_ms"] <= 2**63 - 1
            or type(limits["max_artifact_bytes"]) is not int or not 0 <= limits["max_artifact_bytes"] <= 2**63 - 1):
        raise ValueError("invalid limits")



def reject_constant(value):
    raise ValueError(f"nonfinite JSON number: {value}")


def evaluate_request(request: dict) -> dict:
    """Execute one CLI candidate; geometry, not reported_sum, determines fitness."""
    validate_request(request)
    started = time.monotonic()
    candidate = request["candidate"]
    try:
        code, output, _ = run_candidate(candidate["argv"], candidate["cwd"], b"",
                                        request["limits"]["evaluation_timeout_ms"] / 1000)
        if code:
            raise Rejected(f"candidate_exit: {code}")
        try:
            packing = json.loads(output, object_pairs_hook=unique_object, parse_constant=reject_constant)
            if not isinstance(packing, dict) or set(packing) != {"centers", "radii", "reported_sum"}:
                raise ValueError("expected centers, radii and reported_sum")
        except (ValueError, UnicodeError, RecursionError) as error:
            raise Rejected(f"invalid_output: {error}") from None
        result = assess_packing(**packing)
    except Rejected as error:
        result = _result({"validity": 0.0, "sum_radii": 0.0, "target_ratio": 0.0},
                         {"errors": [str(error)]})
    result.metrics["eval_time"] = time.monotonic() - started
    passed = result.metrics["validity"] == 1.0
    outcome = {"protocol_version": 1, "passed": passed, "metrics": result.metrics}
    if not passed:
        result.metrics.pop("combined_score", None)
        errors = json.loads(result.artifacts["validation.json"])["errors"]
        outcome["reason"] = "; ".join(errors).encode("utf-8")[:4096].decode("utf-8", errors="ignore")
    if request["enable_artifacts"]:
        # Preserve complete JSON artifacts; never truncate their serialized contents.
        budget = request["limits"]["max_artifact_bytes"]
        artifacts = {}
        for key, value in result.artifacts.items():
            size = len(value.encode("utf-8"))
            if size <= budget:
                artifacts[key] = value
                budget -= size
        outcome["artifacts"] = artifacts
    return outcome


def main():
    try:
        text = sys.stdin.buffer.read(1024 * 1024 + 1)
        if len(text) > 1024 * 1024:
            raise ValueError("request too large")
        request = json.loads(text, object_pairs_hook=unique_object, parse_constant=reject_constant)
        json.dump(evaluate_request(request), sys.stdout, allow_nan=False)
        return 0
    except Exception as error:
        print(f"Evaluator infrastructure error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
