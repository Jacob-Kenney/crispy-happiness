import asyncio
from contextlib import asynccontextmanager
import json
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

import play_mcp as p


def response(terminated=False):
    vector = [0.0] * 33
    vector[:4] = [.8, 1.0, .1, .5]
    vector[15:24] = [1, 0, 0, 0, 1, 0, -.1, .2, .3]
    return {"observation": {"dtype": "float32", "shape": [33], "data": vector},
            "terminated": terminated, "info": {"score": 10, "level": 1}}


class ProtocolTests(unittest.TestCase):
    def test_button_order(self):
        self.assertEqual(p.buttons("left"), [0., 1., 0., 0., 0., 0., 0.])
        self.assertEqual(p.buttons("a"), [0., 0., 0., 0., 1., 0., 0.])
        self.assertEqual(p.buttons("wait"), [0.] * 7)
        with self.assertRaises(ValueError):
            p.buttons("jump please")

    def test_observation_contract(self):
        text = p.observation_text(response())
        self.assertIn("ship x=80.0 y=100.0 z=10.0 vz=1.50", text)
        self.assertIn("obstacle full dl=-10.0 dr=20.0 dy=30.0", text)
        bad = response()
        bad["observation"]["shape"] = [34]
        with self.assertRaises(ValueError):
            p.observation_text(bad)

    def test_sdk_response_variants_and_error(self):
        for key in ("structuredContent", "structured_content"):
            self.assertEqual(p.unpack(SimpleNamespace(**{key: response()})), response())
        result = SimpleNamespace(isError=False, structuredContent=None,
                                 content=[SimpleNamespace(type="text", text=json.dumps(response()))])
        self.assertEqual(p.unpack(result), response())
        result.isError = True
        with self.assertRaises(RuntimeError):
            p.unpack(result)

    def test_v15_compact_observation(self):
        text = p.observation_text(response(), compact=True)
        self.assertIn("x=80 y=100 z=10 jump lv=1 sc=10", text)
        self.assertIn("canyon 80..112 dx=-16 gl=8 gr=16", text)
        self.assertIn("obs dy30 full 70..100 dl-10 dr20", text)
        self.assertNotIn("inv=", text)

    def test_v15_endpoint_prompt(self):
        args = SimpleNamespace(model=p.V15_MODEL, base_url="http://localhost:8001/v1", request_timeout=10)
        class Reply:
            def __enter__(self):
                import io
                return io.StringIO(json.dumps({"choices": [{"message": {"content": "a"}}]}))
            def __exit__(self, *_): pass
        with patch.object(p, "urlopen", return_value=Reply()) as request:
            self.assertEqual(p.EndpointPolicy(args)("x=80", "wait"), "a")
            payload = json.loads(request.call_args.args[0].data)
            self.assertEqual(payload["messages"][0]["content"], p.V15_SYSTEM)
            self.assertEqual(payload["messages"][1]["content"],
                             "State:\nx=80\nlast=wait\nReply with one word: left, right, up, down, a, b, wait.")

    def test_client_loop_stops_on_game_over(self):
        calls = []

        class Session:
            def __init__(self, *_): pass
            async def __aenter__(self): return self
            async def __aexit__(self, *_): pass
            async def initialize(self): pass
            async def call_tool(self, tool, arguments):
                calls.append((tool, arguments))
                return SimpleNamespace(structuredContent=response(tool == "step" and len(calls)-1 >= game_length))

        @asynccontextmanager
        async def http_client(url):
            self.assertEqual(url, "http://test/mcp")
            yield (None, None, None)

        mcp = ModuleType("mcp")
        mcp.ClientSession = Session
        http = ModuleType("mcp.client.streamable_http")
        http.streamable_http_client = http_client
        for limit, game_length, expected in ((10, 1, 1), (0, 1502, 1502), (3, 10, 3)):
            with self.subTest(limit=limit), tempfile.TemporaryDirectory() as tmp:
                calls.clear()
                args = SimpleNamespace(mcp_url="http://test/mcp", seed=7, steps=limit,
                                       trace=Path(tmp) / "trace.jsonl")
                modules = {"mcp": mcp, "mcp.client": ModuleType("mcp.client"),
                           "mcp.client.streamable_http": http}
                with patch.dict(sys.modules, modules):
                    asyncio.run(p.play(args, lambda text, last: "a"))
                self.assertEqual(calls[0], ("reset", {"seed": 7}))
                self.assertEqual(calls[1:], [("step", {"action": p.buttons("a")})] * expected)
                self.assertEqual(len(args.trace.read_text().splitlines()), expected + 1)

    def test_gguf_groups_probability_mass_and_restores_prior(self):
        policy = p.GGUFPolicy(SimpleNamespace(base_url="http://localhost:8001/v1", request_timeout=10))
        waits = [t for t, g in policy.groups.items() if g == 0]
        right = next(t for t, g in policy.groups.items() if g == 2)
        probs = [{"id": t, "prob": .3 if t in waits[:2] else .5 if t == right else 0.0}
                 for t in policy.groups]
        with patch.object(policy, "request", side_effect=[{"prompt": "formatted"},
                          {"completion_probabilities": [{"top_probs": probs}]}]) as request:
            # No individual wait token beats right; their combined mass does.
            self.assertEqual(policy("observation", "wait"), "wait")
            payload = request.call_args_list[1].args[1]
            biases = dict(payload["logit_bias"])
            left = next(t for t, g in policy.groups.items() if g == 1)
            self.assertAlmostEqual(biases[left], 100 - 4.839412689208984)
            self.assertEqual(payload["temperature"], 1)
            self.assertEqual(payload["top_k"], 0)
            self.assertTrue(payload["post_sampling_probs"])
            self.assertEqual(payload["n_probs"], len(policy.groups))

    def test_gguf_rejects_incomplete_probabilities(self):
        policy = p.GGUFPolicy(SimpleNamespace())
        with patch.object(policy, "request", side_effect=[{"prompt": "x"},
                          {"completion_probabilities": [{"top_probs": []}]}]):
            with self.assertRaises(ValueError):
                policy("observation", "wait")


if __name__ == "__main__":
    unittest.main()
