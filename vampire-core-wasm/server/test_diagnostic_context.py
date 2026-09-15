import unittest

from agent_pipeline import diagnostic_source, process_log, source_snapshot


class DiagnosticContextTests(unittest.TestCase):
    def test_quadtree_uses_real_implementation(self):
        context = diagnostic_source(source_snapshot(), "QuadTree")
        self.assertIn("EngineCore::updateRepulsionQuadTree", context)
        self.assertIn("void QuadTree::query", context)

    def test_processor_excludes_other_modes(self):
        records = [{"collisionMode": "QuadTree", "memoryMode": "SoA",
                    "distribution": "Clustered", "slots": 500, "frames": 60,
                    "simMs": {"p50": v, "p99": v + 2}, "frameMs": {"p50": v + 1}}
                   for v in (4, 5, 6)]
        records.append({**records[0], "collisionMode": "BruteForce", "slots": 9999})
        observations = process_log(records, "QuadTree", "SoA", "Clustered")
        self.assertEqual(observations["records"], 3)
        self.assertEqual(observations["simMedianMs"], 5)
        self.assertEqual(observations["entityMax"], 500)


if __name__ == "__main__":
    unittest.main()
