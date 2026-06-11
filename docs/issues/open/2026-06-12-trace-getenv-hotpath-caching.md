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

## Verification

## Decision Log
