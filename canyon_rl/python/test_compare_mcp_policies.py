import unittest

from compare_mcp_policies import summarize


class SummaryTests(unittest.TestCase):
    def test_paired_summary_counts_each_seed_once(self):
        rows = []
        for seed, model, reference in ((0, 290, 230), (1, 20, 20), (2, 10, 50)):
            for policy, score in (("v1.5", model), ("reference", reference)):
                rows.append({"seed": seed, "policy": policy, "best_score": score,
                             "game_over": seed == 2, "replay_verified": True})
        result = summarize(rows, [0, 1, 2])
        self.assertEqual(result["wins_ties_losses"], [1, 1, 1])
        self.assertEqual(result["v1.5"]["median"], 20)
        self.assertAlmostEqual(result["v1.5"]["mean"], 320 / 3)
        self.assertEqual(result["reference"]["median"], 50)
        self.assertEqual(result["v1.5"]["alive_at_budget"], 2)
        self.assertEqual(result["v1.5"]["replay_verified"], 3)
        self.assertEqual([r["delta"] for r in result["paired"]], [60, 0, -40])


if __name__ == "__main__":
    unittest.main()
