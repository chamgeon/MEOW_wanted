import unittest

from optimization_agent import ROOT, SOURCE, _generate_candidate, _summary


class OptimizationAgentTests(unittest.TestCase):
    def test_generator_changes_only_selected_function(self):
        original = (ROOT / SOURCE).read_text(encoding="utf-8")
        candidate = _generate_candidate(original)
        self.assertNotEqual(original, candidate)
        self.assertIn("updateRepulsionQuadTree(dt);", candidate)
        _, original_tail = original.split("void EngineCore::updateRepulsionQuadTree(float dt) {", 1)
        _, candidate_tail = candidate.split("void EngineCore::updateRepulsionQuadTree(float dt) {", 1)
        self.assertEqual(original_tail, candidate_tail)

    def test_summary_reports_middle_and_tail(self):
        self.assertEqual(_summary([1.0, 2.0, 3.0])["medianMs"], 2.0)
        self.assertEqual(_summary([1.0, 2.0, 3.0])["p99Ms"], 2.98)


if __name__ == "__main__":
    unittest.main()
