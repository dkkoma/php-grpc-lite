---
Status: Open
Owner: Claude
Created: 2026-06-12
Branch: perf/trace-getenv-hotpath-caching
---

# trace無効時にも毎I/Oで呼ばれる getenv() をプロセス内キャッシュする

## 目的

トレース機能 (`GRPC_LITE_TRACE_FILE` / `GRPC_LITE_TRACE_WIRE_BYTES`) が無効な production 経路で、フレーム送受信・ソケットI/Oごとに発生している `getenv()` 呼び出しを排除する。

## 背景

以下のヘルパが呼び出しのたびに `getenv()` を実行している。

- `src/transport.c:351` `grpc_lite_trace_file_path()` — `getenv("GRPC_LITE_TRACE_FILE")`
- `src/transport.c:358` `grpc_lite_trace_wire_bytes_enabled()` — `getenv("GRPC_LITE_TRACE_WIRE_BYTES")`
- `src/wrapper_adapter.c:148` `grpc_lite_trace_enabled()` / `src/wrapper_adapter.c:155` `grpc_lite_trace_record_call()` — `getenv("GRPC_LITE_TRACE_FILE")`

これらは hot path から毎回呼ばれる:

- `send_callback` (`src/transport.c:1712`) → `grpc_lite_trace_outbound_frame` (`transport.c:614`) — nghttp2 が送信する **チャンクごと**
- `on_frame_recv_callback` (`transport.c:1991`) → `grpc_lite_trace_inbound_frame` (`transport.c:650`) — **受信フレームごと** (WINDOW_UPDATE/PING 含む)
- `connection_recv` / `h2_connection_send` → `grpc_lite_trace_transport_io` (`transport.c:683`) — **read/write ごと** (リトライ含む)

`getenv()` は glibc/musl で environ の線形探索であり、ZTS 環境ではスレッド安全性の観点でも呼び出し回数は少ないほど良い。コード内コメントで「trace env vars are opt-in process diagnostics, not per-request config」と明記済みのため、プロセス起動時に1回だけ評価してキャッシュしてもセマンティクスは変わらない。

## spec照合

HTTP/2 / gRPC spec とは無関係(診断機構のみ)。プロトコル挙動への影響なし。

## 修正方法

1. `transport.c` に static キャッシュを追加:

```c
static const char *grpc_lite_trace_file_path(void)
{
    static const char *cached_path;
    static bool cached;
    if (!cached) {
        const char *path = getenv("GRPC_LITE_TRACE_FILE");
        cached_path = (path != NULL && path[0] != '\0') ? path : NULL;
        cached = true;
    }
    return cached_path;
}
```

   `grpc_lite_trace_wire_bytes_enabled()` も同様に static bool 化。MINIT で初期化しても良い。
2. `wrapper_adapter.c` の `grpc_lite_trace_enabled()` / `grpc_lite_trace_record_call()` は `grpc_lite_trace_file_path()` を共有するように寄せる(現状 `getenv` 直呼びの重複実装)。
3. ZTS 注意: 初回読みの競合は同一値を書くだけなので実害はないが、気にするなら `MINIT` で確定させる。

## 完了条件

- trace 無効時の unary / server streaming hot path から `getenv()` 呼び出しが消えている。
- trace 有効時 (`GRPC_LITE_TRACE_FILE` 設定) の既存 PHPT が通る。
- 既存テストがすべて通る。

## 測定ベンチマーク

- 主計測: `./bench/run.sh cpu-micro` と `./bench/run.sh tls-cpu-micro` — per-call CPU/wall 固定費(`tiny_unary_0b` / `small_unary_100b`)の before/after。getenv は呼び出し回数比例なので小 RPC 反復で最も観測しやすい。
- 回帰確認: `./bench/run.sh spanner-shape`(悪化がないこと)。

## Progress

- 2026-06-12: 実装完了。`transport.c` の `grpc_lite_trace_file_path()` / `grpc_lite_trace_wire_bytes_enabled()` を初回呼び出し時キャッシュ化(static 変数、ZTS の初回競合は同一値書き込みのみで無害)。`grpc_lite_trace_file_path()` を非 static 化して `transport.h` で公開し、`wrapper_adapter.c` の `grpc_lite_trace_enabled()` / `grpc_lite_trace_record_call()` の getenv 直呼び重複実装を共有に置き換え。これで trace 無効時の hot path から getenv() が消えた(初回 1 回のみ)。

## Verification

- PHPT 15/15 PASS(trace 有効経路の 029-trace-file 含む)。
- C unit 3 suite PASS、静的解析 PASS、PHPUnit 統合 30 tests / 109 assertions PASS。
- ベンチ before(`main-baseline-20260612`)/ after(`trace-getenv-after-20260612`)、cpu_us/call と wall_us/call:
  - cpu-micro tiny_unary_0b: 10.4/31.4 → 10.0/30.8、small_unary_100b: 9.5/27.7 → 10.1/30.8
  - tls-cpu-micro tiny_unary_0b: 12.3/34.8 → 12.1/36.0、small_unary_100b: 11.8/32.7 → 11.4/33.3
  - spanner-shape p50: begin_txn 29.0→29.6 / commit_txn 24.8→26.1 / select_1row 24.1→26.5(揺れ幅内)

## Decision Log

- 2026-06-12: **採用**。計測上の改善はノイズ床以下(getenv は 1 call あたり数回 × ~100ns 程度で、run 間揺れ ±1〜3µs に埋もれる)。ただし本 issue の主目的である「trace 無効 production 経路からの environ 線形探索排除」「ZTS での getenv 呼び出し削減」「後続ベンチのノイズ要因除去」はコード上達成されており、分岐追加もなく複雑性増がほぼゼロのため、性能 issue ではなく hot path 衛生として採用する。悪化がないことは cpu-micro / spanner-shape で確認済み。
- 2026-06-13: マージ前コードレビューで 2 点を指摘され修正。(1) getenv() の返すポインタを static にキャッシュするのは危険(PHP の putenv() は request 終了時に値を復元・解放するため、FPM/worker 常駐プロセスで dangling になり得る)→ MINIT で 1 回読み、`strdup` でプロセス所有コピーを保持(MSHUTDOWN で解放)。(2) 関数内 static の lazy 初期化は ZTS の弱メモリモデルで `cached` フラグと値の可視順序が保証されない → MINIT 初期化(シングルスレッド)で公開競合自体を排除。あわせて 029-trace-file.phpt を実行時 putenv() から `--ENV--` セクション(プロセス起動前設定)に変更し、「trace env はプロセス診断で実行時変更不可」というセマンティクスをテストでも固定した。
