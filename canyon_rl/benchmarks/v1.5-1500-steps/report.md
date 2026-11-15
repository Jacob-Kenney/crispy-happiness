# v1.5 GGUF versus reference heuristic

Ten paired seeds (0–9), maximum 1,500 actions / 6,000 game frames per episode. Both policies use the canonical canyon-jumpeRL MCP EmulatorBackend, with identical resets, observations and four-frame actions. Evaluation calls the backend directly, without MCP HTTP or Harbor containers. The v1.5 seed-0 result matches the independently verified Harbor reward of 290.

The reference is upstream HeuristicPolicy(th=30, hold=2), a jump-timing heuristic. v1.5 uses the published Q4_K_M weights with recovered grouped-token action selection and learned action biases. Policies were held fixed across all seeds. Score is the best raw in-game score, including before rewinds; this is the same reward definition as Harbor.

| Metric | v1.5 | Reference |
|---|---:|---:|
| Mean score | 170 | 146 |
| Median score | 145 | 195 |
| Score range | 60–380 | 20–250 |
| Alive at budget | 10/10 | 7/10 |

v1.5 won 6 seeds and lost 4, with no ties. Its mean score was 16.4% higher, but its median was lower. The mean advantage partly reflects avoiding the reference's three game-over runs with score 20. The original score of 290 was above v1.5's typical result in this sample. This is a modest, uneven improvement against a simple heuristic, not evidence of consistently strong play.

All 20 action traces were independently replayed in fresh emulators. Replay checked best score, action count, final score, lives and frames exactly. The source, model and decoder hashes are recorded in provenance.json.

| Seed | v1.5 | Reference | Difference |
|---:|---:|---:|---:|
| 0 | 290 | 230 | +60 |
| 1 | 110 | 20 | +90 |
| 2 | 380 | 250 | +130 |
| 3 | 160 | 220 | -60 |
| 4 | 180 | 190 | -10 |
| 5 | 190 | 20 | +170 |
| 6 | 90 | 80 | +10 |
| 7 | 130 | 200 | -70 |
| 8 | 110 | 230 | -120 |
| 9 | 60 | 20 | +40 |

These are turn-based, budget-limited episodes, not real-time latency tests or full-game averages. Ten seeds give a useful baseline comparison, not a precise estimate of general performance.

Files: summary.json, scores.csv, episodes.json, config.json, provenance.json, and per-policy/per-seed JSONL action traces.
