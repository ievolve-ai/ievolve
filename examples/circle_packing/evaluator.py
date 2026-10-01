"""Validate 26 circles without an OpenEvolve runtime dependency.

The C++ evaluator owns process isolation and the timeout, including import time.
This module does not start a second process. Geometry uses absolute tolerance
1e-12 in unit-square coordinates: a few orders above float64 rounding in the
slack arithmetic (~1e-16), so tangent circles are not rejected, yet too small
for inflated radii to buy a meaningful score. Negative radii are never tolerated.
"""

import importlib.util
import json
import math
from pathlib import Path
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


def evaluate(program_path):
    """Import a candidate and call run_packing() once inside the C++ worker."""
    started = time.monotonic()
    candidate_path = Path(program_path).resolve()
    module_name = "_ievolve_circle_packing_candidate"
    previous_path = list(sys.path)
    previous_module = sys.modules.get(module_name)
    try:
        sys.path.insert(0, str(candidate_path.parent))
        spec = importlib.util.spec_from_file_location(module_name, candidate_path)
        if spec is None or spec.loader is None:
            raise ImportError(f"Cannot load candidate: {candidate_path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        if not callable(getattr(module, "run_packing", None)):
            raise TypeError("Candidate must define run_packing()")
        values = module.run_packing()
        if not isinstance(values, (tuple, list)) or len(values) != 3:
            raise ValueError("run_packing() must return (centers, radii, reported_sum)")
        result = assess_packing(*values)
    except Exception as error:
        result = _result(
            {"validity": 0.0, "sum_radii": 0.0, "target_ratio": 0.0,
             "combined_score": 0.0},
            {"errors": [f"{type(error).__name__}: {error}"]},
        )
    finally:
        sys.path[:] = previous_path
        if previous_module is None:
            sys.modules.pop(module_name, None)
        else:
            sys.modules[module_name] = previous_module
    result.metrics["eval_time"] = time.monotonic() - started
    return result
