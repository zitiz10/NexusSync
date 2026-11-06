# Evaluation results

`python3 eval/run_eval.py eval/datasets` writes here: `summary.md` (tables, hardware, notes), `storage.csv`, `uploads.csv`, `downloads.csv`, `recovery.csv`, `checkpoints.csv`, `chunk_sizes.csv`, and four charts as PNG and SVG (`dedup_ratio`, `network_sent`, `chunk_sizes`, `recovery`).

Commit the results of the real lab datasets used in the report. The synthetic demo data (`scripts/make_demo_data.py`) is only a smoke test of the tooling; do not present its numbers as lab measurements.
