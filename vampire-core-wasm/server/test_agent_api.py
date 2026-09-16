import unittest
from unittest.mock import AsyncMock, patch

from fastapi.testclient import TestClient

import main
from agent_pipeline import source_hash, source_snapshot


class AgentApiTests(unittest.TestCase):
    def setUp(self):
        main.optimization_jobs.clear()
        self.client = TestClient(main.app)
        self.records = [{"collisionMode": "BruteForce", "memoryMode": "SoA",
                         "distribution": "Uniform", "slots": 500, "frames": 60,
                         "simMs": {"p50": 5, "p99": 7}, "frameMs": {"p50": 6}}
                        for _ in range(3)]

    def request(self):
        return {"collisionMode": "BruteForce", "memoryMode": "SoA",
                "distribution": "Uniform", "sourceHash": source_hash(source_snapshot()),
                "telemetry": {"records": self.records}}

    def test_rejects_stale_source_before_creating_job(self):
        request = self.request()
        request["sourceHash"] = "stale"
        response = self.client.post("/api/optimization-jobs", json=request)
        self.assertEqual(response.status_code, 409)

    def test_rejects_mixed_or_insufficient_history(self):
        request = self.request()
        request["telemetry"]["records"] = self.records[:2]
        self.assertEqual(self.client.post("/api/optimization-jobs", json=request).status_code, 422)
        request["telemetry"]["records"] = [dict(r, collisionMode="QuadTree") for r in self.records]
        self.assertEqual(self.client.post("/api/optimization-jobs", json=request).status_code, 422)

    def test_creates_async_job_and_exposes_status(self):
        with patch.object(main, "_process_optimization", new_callable=AsyncMock):
            response = self.client.post("/api/optimization-jobs", json=self.request())
        self.assertEqual(response.status_code, 202)
        job_id = response.json()["jobId"]
        self.assertEqual(self.client.get(f"/api/optimization-jobs/{job_id}").json()["status"], "queued")
        self.assertEqual(self.client.get(f"/api/optimization-jobs/{job_id}/artifacts/core_engine.js").status_code, 404)

    def test_accepts_new_collision_modes(self):
        for mode in ("UniformGrid", "SpatialHash"):
            with self.subTest(mode=mode), patch.object(main, "_process_optimization", new_callable=AsyncMock):
                request = self.request()
                request["collisionMode"] = mode
                request["telemetry"]["records"] = [dict(record, collisionMode=mode) for record in self.records]
                self.assertEqual(self.client.post("/api/optimization-jobs", json=request).status_code, 202)

    def test_speed_must_match_telemetry(self):
        request = self.request()
        request["speedMultiplier"] = 2.0
        self.assertEqual(self.client.post("/api/optimization-jobs", json=request).status_code, 422)
        request["telemetry"]["records"] = [dict(record, speedMultiplier=2.0) for record in self.records]
        with patch.object(main, "_process_optimization", new_callable=AsyncMock):
            self.assertEqual(self.client.post("/api/optimization-jobs", json=request).status_code, 202)

    def test_rejects_bruteforce_above_safe_limit(self):
        request = self.request()
        request["telemetry"]["records"] = [dict(record, slots=20000) for record in self.records]
        response = self.client.post("/api/optimization-jobs", json=request)
        self.assertEqual(response.status_code, 422)
        self.assertIn("10,000", response.text)

    def test_accepts_low_fps_hundred_thousand_grid_log(self):
        request = self.request()
        request["collisionMode"] = "UniformGrid"
        request["telemetry"]["records"] = [dict(record, collisionMode="UniformGrid",
            slots=100000, frames=1, fps=1.0) for record in self.records]
        with patch.object(main, "_process_optimization", new_callable=AsyncMock):
            self.assertEqual(self.client.post("/api/optimization-jobs", json=request).status_code, 202)

    def test_queue_cap(self):
        with patch.object(main, "_process_optimization", new_callable=AsyncMock):
            self.assertEqual(self.client.post("/api/optimization-jobs", json=self.request()).status_code, 202)
            self.assertEqual(self.client.post("/api/optimization-jobs", json=self.request()).status_code, 202)
            self.assertEqual(self.client.post("/api/optimization-jobs", json=self.request()).status_code, 429)


if __name__ == "__main__":
    unittest.main()
