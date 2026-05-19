# Diagnostics fixtures

This directory contains ad-hoc investigation fixtures. They are not standard benchmark suites and their output must not be treated as the repository's benchmark baseline.

Standard performance comparisons should continue to use `bench/run.sh` / `bench/compare.sh` and OTEL summaries. Files here are for reproducing a specific production or issue-reported shape with minimal surrounding framework code.

## `spanner-transaction-cli.php`

Runs a real Cloud Spanner high-level PHP client transaction loop:

1. warmup `SELECT 1`,
2. for each iteration, `runTransaction()`,
3. inside the transaction, `SELECT @i`,
4. `commit()`.

The printed wall time is the full transaction iteration, not the `/google.spanner.v1.Spanner/Commit` RPC alone. To inspect per-RPC timing and HTTP/2 wire shape, run it with `GRPC_LITE_TRACE_FILE` and analyze the emitted `rpc.end`, `wire.request_header`, `wire.frame_out`, and `wire.frame_in` records.
