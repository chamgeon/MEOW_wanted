import unittest
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

import main


class OpenAIEndpointTests(unittest.IsolatedAsyncioTestCase):
    async def test_new_advisor_ranks_supported_candidates_only(self):
        create = AsyncMock(return_value=SimpleNamespace(
            output_text='{"candidates":["SpatialHash","Unexpected"],"reason":"test"}'))
        client = SimpleNamespace(responses=SimpleNamespace(create=create))
        request = main.OptimizationJobRequest(
            collisionMode="BruteForce", memoryMode="SoA", distribution="Uniform",
            sourceHash="x", telemetry={"records": []},
        )
        with patch.dict(main.os.environ, {"OPENAI_API_KEY": "test"}), \
             patch.object(main.openai, "AsyncOpenAI", return_value=client):
            order, advisor_mode, advice = await main._plan(
                request, {"simMedianMs": 4}, main.source_snapshot())
        self.assertEqual(order, ["SpatialHash", "QuadTree", "UniformGrid"])
        self.assertEqual(advisor_mode, "OpenAI advisor")
        self.assertEqual(advice, "test")

    async def test_legacy_analysis_still_uses_its_existing_contract(self):
        create = AsyncMock(return_value=SimpleNamespace(
            content=[SimpleNamespace(type="text", text="diagnosis")]))
        client = SimpleNamespace(messages=SimpleNamespace(create=create))
        request = main.OptimizeRequest(
            fps=42.0, frameTimeMs=22.0, entityCount=5000,
            collisionMode="BruteForce", memoryMode="SoA", codeSnippet="test loop")
        with patch.object(main, "get_client", return_value=client):
            result = await main.optimize(request)
        self.assertEqual(result.analysis, "diagnosis")


if __name__ == "__main__":
    unittest.main()
