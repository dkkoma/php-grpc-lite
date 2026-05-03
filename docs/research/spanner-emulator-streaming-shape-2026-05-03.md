# Spanner Emulator Streaming Shape 2026-05-03

## Purpose

Spanner emulator の `ExecuteStreamingSql` が、結果セット形状に応じてどのような `PartialResultSet` message列を返すかを実測した。

transport性能比較ではなく、small SELECT benchmarkのpayload/message-count前提を確認するための観測である。実行経路は既存互換性検証済みのcurl transportに固定した。

## Command

```bash
docker compose run --rm -e PHP_GRPC_LITE_TRANSPORT=curl dev \
  php tools/phase2/spanner-streaming-shape.php \
  --output=var/bench-results/spanner-streaming-shape-20260503.json
```

`var/bench-results/` は作業用出力なのでgit管理しない。代表値はこのdocへ転記する。

## Results

| case | partials | estimated rows | values | serialized bytes | first partial bytes | max partial bytes | chunked partials |
|---|---:|---:|---:|---:|---:|---:|---:|
| one_row_one_int64 | 1 | 1 | 1 | 18 | 18 | 18 | 0 |
| one_row_spanner_like_10_columns | 1 | 1 | 10 | 267 | 267 | 267 | 0 |
| ten_rows_one_int64 | 1 | 10 | 10 | 64 | 64 | 64 | 0 |
| ten_rows_spanner_like_10_columns | 1 | 10 | 100 | 1195 | 1195 | 1195 | 0 |
| hundred_rows_spanner_like_10_columns | 1 | 100 | 1000 | 10556 | 10556 | 10556 | 0 |
| thousand_rows_one_int64 | 1 | 1000 | 1000 | 6906 | 6906 | 6906 | 0 |
| one_row_1k_string | 1 | 1 | 1 | 1049 | 1049 | 1049 | 0 |
| one_row_10k_string | 1 | 1 | 1 | 10265 | 10265 | 10265 | 0 |

`one_row_spanner_like_10_columns` の列構成:

- `id INT64`
- `created_date DATE`
- `updated_date DATE`
- `first_text STRING`
- `second_text STRING`
- `active BOOL`
- `score INT64`
- `ratio FLOAT64`
- `event_ts TIMESTAMP`
- `payload BYTES`

## Interpretation

- emulatorでは、この範囲の小さい `ExecuteStreamingSql` はすべて1つの `PartialResultSet` で返った。
- 2 DATE + 2 STRING + 合計10 columnsの1行SELECTは、protobuf serialized payloadで267 bytesだった。
- 同じ10 columnsでも10行で約1.2KiB、100行で約10.6KiBだった。少なくともemulatorでは10行程度は「10 messages」ではなく「1 message内に100 values」として返る。
- `1000 rows x 1 INT64` も1 messageだったため、message分割をSpanner emulatorの小型SELECTから自然に引き出すのは難しい。
- small SELECT benchmarkの主代表は `1x100b` より `1x1k` に近いが、今回の具体的な10 columns / 1 rowは267Bなので、`1x100b` と `1x1k` の間にある。

## Benchmark Implication

- small SELECT benchmarkは1 messageケースだけに絞る方針でよい。
- `1x100b` / `1x1k` / `1x4k` / `1x10k` は、Spanner emulator観測から見てもmessage countではなくpayload size軸として扱う。
- 複数message分割、chunked value、long streamはSpanner small SELECT軸に混ぜず、many-small / large streaming / chunking専用の別ケースで測る。
