import unittest

from agent_candidates import ENGINE, TREE, comparison_algorithms, eligible, generate
from agent_pipeline import _compare_world, comparison_matrix, process_log, source_hash, source_snapshot


class AgentPipelineTests(unittest.TestCase):
    def test_three_candidates_are_independent_and_leave_baseline_untouched(self):
        baseline = source_snapshot()
        before = source_hash(baseline)
        for algorithm in eligible("BruteForce"):
            modified = generate(baseline, "BruteForce", algorithm)
            self.assertNotEqual(modified[ENGINE], baseline[ENGINE])
            self.assertEqual(modified[TREE], baseline[TREE])
        self.assertEqual(source_hash(baseline), before)

    def test_quadtree_can_be_optimized_too(self):
        baseline = source_snapshot()
        modified = generate(baseline, "QuadTree", "QuadTreeReuse")
        self.assertEqual(modified[ENGINE], baseline[ENGINE])
        self.assertNotEqual(modified[TREE], baseline[TREE])

    def test_world_gate_rejects_different_kills(self):
        reference = {"alive": 2, "kills": 3, "positions": [[1, 2], [3, 4]]}
        altered = {**reference, "kills": 4}
        self.assertFalse(_compare_world(reference, altered)["passed"])

    def test_quadtree_comparison_covers_six_other_combinations(self):
        self.assertEqual(comparison_algorithms("QuadTree"),
                         ["BruteForce", "UniformGrid", "SpatialHash"])
        matrix = comparison_matrix("QuadTree", ["SpatialHash"])
        self.assertEqual(len(matrix), 6)
        self.assertEqual({(item["algorithm"], item["memoryMode"]) for item in matrix},
                         {(algorithm, memory)
                          for algorithm in ("BruteForce", "UniformGrid", "SpatialHash")
                          for memory in ("AoS", "SoA")})
        self.assertEqual({item["runMode"] for item in matrix if item["algorithm"] == "BruteForce"},
                         {"BruteForce"})

    def test_bruteforce_comparison_keeps_both_memory_layouts(self):
        matrix = comparison_matrix("BruteForce", ["QuadTree", "QuadTree"])
        self.assertEqual(len(matrix), 6)
        self.assertEqual({item["algorithm"] for item in matrix},
                         {"QuadTree", "UniformGrid", "SpatialHash"})

    def test_each_active_collision_mode_excludes_itself_and_compares_six(self):
        all_modes = {"BruteForce", "QuadTree", "UniformGrid", "SpatialHash"}
        for mode in all_modes:
            with self.subTest(mode=mode):
                matrix = comparison_matrix(mode, [])
                self.assertEqual(len(matrix), 6)
                self.assertEqual({item["algorithm"] for item in matrix}, all_modes - {mode})
                self.assertTrue(all(item["runMode"] == item["algorithm"] for item in matrix))

    def test_log_processor_does_not_mix_game_speeds(self):
        row = {"collisionMode": "UniformGrid", "memoryMode": "SoA", "distribution": "Uniform",
               "speedMultiplier": 2.0, "slots": 100000, "frames": 10, "fps": 5.0,
               "simMs": {"p50": 200.0, "p99": 250.0}, "frameMs": {"p50": 220.0}}
        observations = process_log([row] * 3 + [dict(row, speedMultiplier=1.0)] * 3,
                                   "UniformGrid", "SoA", "Uniform", 2.0)
        self.assertEqual(observations["records"], 3)
        self.assertEqual(observations["entityMax"], 100000)
        self.assertEqual(observations["speedMultiplier"], 2.0)


if __name__ == "__main__":
    unittest.main()
