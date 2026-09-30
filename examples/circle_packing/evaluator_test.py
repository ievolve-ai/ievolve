"""Offline regression tests for the migrated circle-packing example."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

import numpy as np

import check_packing
from evaluator import assess_packing, evaluate, REFERENCE_SUM, TOLERANCE
from initial_program import run_packing


class PackingEvaluatorTest(unittest.TestCase):
    def setUp(self):
        # Well-separated circles, so changing one constraint tests that constraint.
        self.centers = np.array(
            [[(column + 0.5) / 6, (row + 0.5) / 5]
             for row in range(5) for column in range(6)][:26]
        )
        self.radii = np.full(26, 0.04)

    def assert_invalid(self, centers, radii, error_fragment):
        result = assess_packing(centers, radii, 999)
        self.assertEqual(result.metrics["validity"], 0.0)
        self.assertEqual(result.metrics["sum_radii"], 0.0)
        self.assertEqual(result.metrics["target_ratio"], 0.0)
        self.assertEqual(result.metrics["combined_score"], 0.0)
        diagnostics = json.loads(result.artifacts["validation.json"])
        self.assertIn(error_fragment, " ".join(diagnostics["errors"]))
        # Match the worker's strict JSON serialization contract.
        json.dumps(result.metrics, allow_nan=False)
        for artifact in result.artifacts.values():
            json.loads(artifact, parse_constant=lambda value: self.fail(value))
        return result

    def test_original_baseline_is_valid(self):
        centers, radii, reported_sum = run_packing()
        result = assess_packing(centers, radii, reported_sum)
        self.assertEqual(result.metrics["validity"], 1.0)
        self.assertEqual(centers.shape, (26, 2))
        self.assertEqual(radii.shape, (26,))
        self.assertAlmostEqual(result.metrics["sum_radii"], float(radii.sum()), places=14)
        self.assertAlmostEqual(result.metrics["combined_score"], float(radii.sum()), places=14)
        self.assertAlmostEqual(result.metrics["target_ratio"], float(radii.sum()) / REFERENCE_SUM)
        np.testing.assert_array_equal(centers[25], [0.01, 0.01])

    def test_exact_circle_count_is_required(self):
        for count in (0, 1, 25, 27):
            with self.subTest(count=count):
                self.assert_invalid(np.zeros((count, 2)), np.zeros(count), "shape")

    def test_center_shape_is_checked_before_geometry(self):
        for centers in (0.5, np.zeros(26), np.zeros((26, 1)), np.zeros((26, 3)), np.zeros((2, 26))):
            with self.subTest(shape=np.shape(centers)):
                self.assert_invalid(centers, self.radii, "shape")

    def test_radius_shape_is_checked_before_geometry(self):
        for radii in (0.1, np.zeros(25), np.zeros((26, 1)), np.zeros((1, 26))):
            with self.subTest(shape=np.shape(radii)):
                self.assert_invalid(self.centers, radii, "shape")

    def test_ragged_arrays_are_rejected(self):
        self.assert_invalid([[0.1, 0.2], [0.3]], self.radii, "convert")

    def test_non_numeric_and_complex_arrays_are_rejected(self):
        for centers in (self.centers.astype(str), self.centers.astype(complex)):
            with self.subTest(dtype=str(centers.dtype)):
                self.assert_invalid(centers, self.radii, "real numeric")

    def test_nonfinite_centers_are_rejected(self):
        for number in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(number=number):
                centers = self.centers.copy()
                centers[0, 0] = number
                self.assert_invalid(centers, self.radii, "finite")

    def test_nonfinite_radii_are_rejected(self):
        for number in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(number=number):
                radii = self.radii.copy()
                radii[0] = number
                self.assert_invalid(self.centers, radii, "finite")

    def test_any_negative_radius_is_rejected(self):
        radii = self.radii.copy()
        radii[0] = -1e-12
        result = self.assert_invalid(self.centers, radii, "Negative radii")
        self.assertEqual(result.metrics["negative_radius_count"], 1)

    def test_overlap_is_rejected_with_residual(self):
        centers = self.centers.copy()
        centers[1] = centers[0]
        result = self.assert_invalid(centers, self.radii, "overlap")
        self.assertAlmostEqual(result.metrics["max_overlap"], 0.08)
        self.assertEqual(result.metrics["overlap_pair_count"], 1)

    def test_all_four_square_boundaries_are_checked(self):
        for position in ([0.03, 0.1], [0.97, 0.1], [0.1, 0.03], [0.1, 0.97]):
            with self.subTest(position=position):
                centers = self.centers.copy()
                centers[0] = position
                result = self.assert_invalid(centers, self.radii, "exceed the square")
                self.assertAlmostEqual(result.metrics["max_boundary_violation"], 0.01)

    def test_tolerance_accepts_small_boundary_roundoff(self):
        centers = self.centers.copy()
        centers[0] = [self.radii[0] - TOLERANCE / 2, 0.1]
        result = assess_packing(centers, self.radii)
        self.assertEqual(result.metrics["validity"], 1.0)
        self.assertAlmostEqual(result.metrics["min_boundary_slack"], -TOLERANCE / 2)
        centers[0, 0] = self.radii[0] - 2 * TOLERANCE
        self.assert_invalid(centers, self.radii, "exceed the square")

    def test_tolerance_accepts_small_overlap_roundoff(self):
        centers = self.centers.copy()
        centers[1] = centers[0] + [2 * self.radii[0] - TOLERANCE / 2, 0]
        result = assess_packing(centers, self.radii)
        self.assertEqual(result.metrics["validity"], 1.0)
        self.assertAlmostEqual(result.metrics["max_overlap"], TOLERANCE / 2)
        centers[1] = centers[0] + [2 * self.radii[0] - 2 * TOLERANCE, 0]
        self.assert_invalid(centers, self.radii, "overlap")

    def test_spoofed_sum_never_changes_score(self):
        expected = float(self.radii.sum())
        for reported in (99999.0, -1, float("nan"), float("inf"), "not a sum"):
            with self.subTest(reported=reported):
                result = assess_packing(self.centers, self.radii, reported)
                self.assertEqual(result.metrics["validity"], 1.0)
                self.assertAlmostEqual(result.metrics["sum_radii"], expected)
                self.assertAlmostEqual(result.metrics["combined_score"], expected)

    def test_zero_radii_and_list_inputs_are_valid(self):
        result = assess_packing([[0.5, 0.5]] * 26, [0.0] * 26)
        self.assertEqual(result.metrics["validity"], 1.0)
        self.assertEqual(result.metrics["sum_radii"], 0.0)

    def test_overflow_is_rejected_without_nonfinite_json(self):
        self.assert_invalid(np.full((26, 2), 1e308), np.full(26, 1e308), "overflowed")

    def test_artifacts_preserve_geometry_and_all_residuals(self):
        result = assess_packing(self.centers, self.radii, 9999)
        packing = json.loads(result.artifacts["packing.json"])
        diagnostics = json.loads(result.artifacts["validation.json"])
        np.testing.assert_array_equal(packing["centers"], self.centers)
        np.testing.assert_array_equal(packing["radii"], self.radii)
        self.assertEqual(len(diagnostics["boundary_slacks"]), 26)
        self.assertTrue(all(len(row) == 4 for row in diagnostics["boundary_slacks"]))
        self.assertEqual(len(diagnostics["pair_slacks"]), 325)
        self.assertAlmostEqual(diagnostics["reported_sum_error"], 9999 - float(self.radii.sum()))

    def test_evaluate_imports_baseline_without_openevolve(self):
        old_path = list(sys.path)
        result = evaluate(Path(__file__).with_name("initial_program.py"))
        self.assertEqual(result.metrics["validity"], 1.0)
        self.assertGreaterEqual(result.metrics["eval_time"], 0)
        self.assertEqual(sys.path, old_path)
        self.assertNotIn("_ievolve_circle_packing_candidate", sys.modules)

    def test_evaluate_reports_import_function_and_return_errors(self):
        sources = [
            ("raise RuntimeError('broken import')", "broken import"),
            ("answer = 42", "run_packing"),
            ("def run_packing(): return 1, 2", "must return"),
            ("def run_packing(): raise ValueError('bad constructor')", "bad constructor"),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            for index, (source, fragment) in enumerate(sources):
                with self.subTest(source=source):
                    candidate = Path(temporary) / f"candidate_{index}.py"
                    candidate.write_text(source + "\n")
                    result = evaluate(candidate)
                    self.assertEqual(result.metrics["validity"], 0.0)
                    self.assertIn(fragment, result.artifacts["validation.json"])

    def test_headless_check_writes_parseable_svg_and_actual_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = check_packing.main(["--output", temporary])
            self.assertEqual(code, 0)
            output = Path(temporary)
            metrics = json.loads(stdout.getvalue())
            self.assertEqual(metrics, json.loads((output / "metrics.json").read_text()))
            root = ET.parse(output / "packing.svg").getroot()
            self.assertEqual(len(root.findall("{http://www.w3.org/2000/svg}circle")), 26)
            packing = json.loads((output / "packing.json").read_text())
            self.assertAlmostEqual(metrics["sum_radii"], sum(packing["radii"]))

    def test_saved_geometry_check_ignores_saved_sum_and_removes_stale_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "output"
            saved = Path(temporary) / "saved.json"
            saved.write_text(json.dumps({
                "centers": self.centers.tolist(), "radii": self.radii.tolist(),
                "reported_sum": 99999, "actual_sum_radii": 99999,
            }))
            with redirect_stdout(io.StringIO()):
                self.assertEqual(check_packing.main([
                    "--packing-json", str(saved), "--output", str(output)]), 0)
            metrics = json.loads((output / "metrics.json").read_text())
            self.assertAlmostEqual(metrics["sum_radii"], float(self.radii.sum()))
            saved.write_text(json.dumps({"centers": [], "radii": []}))
            with redirect_stdout(io.StringIO()):
                self.assertEqual(check_packing.main([
                    "--packing-json", str(saved), "--output", str(output)]), 1)
            self.assertFalse((output / "packing.svg").exists())
            self.assertFalse((output / "packing.json").exists())


if __name__ == "__main__":
    unittest.main()
