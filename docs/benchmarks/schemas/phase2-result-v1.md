# Phase 2 Result JSON v1

Phase 2 の探索ベンチは、PHPBench aggregate JSON とは別に以下の contract を使う。

```json
{
  "schema": "php-grpc-lite.phase2-benchmark.v1",
  "generated_at": "2026-04-28T00:00:00Z",
  "suite": "contract-smoke",
  "implementation": "php-grpc-lite",
  "environment": {
    "php_version": "8.4.x",
    "php_sapi": "cli",
    "os": "Linux",
    "machine": "aarch64",
    "hostname": "container",
    "opcache_cli": false,
    "xdebug_loaded": false
  },
  "measurements": [
    {
      "name": "contract_smoke_loop",
      "axis": "contract",
      "subject": "integer_loop",
      "attributes": {
        "revs": 100000
      },
      "metrics": {
        "wall_time_ns_total": {
          "value": 1000000,
          "unit": "ns"
        },
        "wall_time_ns_per_op": {
          "value": 10.0,
          "unit": "ns/op"
        }
      }
    }
  ]
}
```

## フィールド

| field | 意味 |
|---|---|
| `schema` | contract 名。互換性を壊す場合は suffix を上げる |
| `generated_at` | UTC の生成時刻 |
| `suite` | runner suite 名。例: `contract-smoke`, `cpu-memory-smoke`, `rtt-unary` |
| `implementation` | `php-grpc-lite` または `ext-grpc` |
| `environment` | 実行環境。比較時の補助情報で、判定 key にはしない |
| `measurements` | 計測行の配列 |

`measurements[].metrics` は metric 名を key にした object とし、各 metric は `value` と任意の `unit` を持つ。Phase 2 では `wall_time_ns_*`、`cpu_*`、`memory_*`、`ops_per_second`、`p50/p95/p99` などを同じ形で追加する。

`cpu_*` は PHP の `getrusage()` から取り、短すぎる処理では分解能の影響を受ける。CPU per call を見る runner は、多数回の operation を 1 sample にまとめてから per-op 指標へ割り戻す。

## 運用

- JSON は `var/bench-results/phase2-<suite>-<tag>-<implementation>.json` に保存する。
- Phase 2 の探索結果は `bench/baselines/regression.json` に混ぜない。
- 結果を採用する場合だけ、環境、代表値、揺れ幅、判断を `docs/benchmarks/multi-axis-2026-XX-XX.md` に記録する。
