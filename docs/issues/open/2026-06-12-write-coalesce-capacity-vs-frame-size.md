---
Status: Open
Owner: Claude
Created: 2026-06-12
Branch: perf/write-coalesce-capacity
Related: docs/issues/open/2026-06-12-h2c-write-coalescing.md
---

# write coalesce バッファ容量 (16KB) が max frame size と同値で、DATA フレームが常にバイパスされる問題

## 目的

`GRPC_LITE_H2_WRITE_COALESCE_CAPACITY` (16384) が HTTP/2 デフォルト max frame size (16384) と同値のため、フルサイズ DATA フレーム (9 + 16384 byte) が必ず coalesce バッファをバイパスし、フレームごとに個別の `SSL_write`(= 個別 TLS record 群 + syscall)になっている問題を解消する。

## 背景

- `src/transport.c:14` `#define GRPC_LITE_H2_WRITE_COALESCE_CAPACITY 16384`
- `h2_connection_buffer_or_write` (`transport.c:1354`): `length > CAPACITY` のチャンクは flush + 直接 `h2_connection_write_all`。

nghttp2 はフルサイズ DATA フレームを 16393 byte(ヘッダ 9 + payload 16384)のチャンクで `send_callback` に渡すため、**16KB を超えるリクエスト送信では全 DATA フレームがバイパス対象**になる。結果:

- 1MB のリクエスト送信 = 約 64 回の `SSL_write`。各 `SSL_write` は独立した TLS record 列を生成(record per-write のオーバーヘッド: 5 byte header + MAC/tag ~17-29 byte + 暗号化処理の固定費)。
- HEADERS と最初の DATA も別 write に分かれる。
- さらに ini `grpc_lite.http2_max_frame_size` を 16384 より大きくすると(`grpc.c:17`、最大 16MB)、**全フレームが常にバイパスされ coalescing が事実上無効化**される。

## spec照合

- RFC 9113 §4.2: SETTINGS_MAX_FRAME_SIZE はフレームサイズの上限交渉であり、複数フレームを 1 回の write/TLS record にまとめることに制約はない。
- RFC 8446 (TLS 1.3): record 上限 16KB は OpenSSL が `SSL_write` 内で自動分割するため、大きい write を渡しても正しく処理される。むしろ 1 回の `SSL_write` に多く渡すほうが record 充填率が上がる。
- gRPC spec: 影響なし。

## 修正方法

1. coalesce 容量を「有効 max frame size に追従」させる:
   - 接続作成時に `effective_http2_max_frame_size()` を基準に `write_buffer_cap = max(4 * (max_frame_size + 9), 65536)` 程度で確保(固定 64KB でも可)。
   - `h2_connection_buffer_or_write` の `length > capacity` 判定は `connection->write_buffer_cap` ベースに変更(現状も実質そうだが、定数 `GRPC_LITE_H2_WRITE_COALESCE_CAPACITY` との比較 `transport.c:1363` を cap 参照に直す)。
2. これにより 1 回の `SSL_write` で複数 DATA フレームがまとまり、大リクエストの `SSL_write` 回数が約 1/4 になる。
3. メモリ増は persistent 接続あたり +48KB 程度。`GRPC_LITE_MAX_PERSISTENT_CONNECTIONS` 上限と掛け算して許容範囲か確認する。
4. ベンチ: 大 payload リクエスト(数百 KB〜1MB)の送信時間と CPU を before/after 比較。

## 完了条件

- フルサイズ DATA フレームが coalesce バッファに乗り、`wire.tls_write` の回数が削減されている。
- `grpc_lite.http2_max_frame_size` を拡大した構成でも coalescing が機能する。
- 既存テスト・ベンチで悪化なし。

## 測定ベンチマーク

- 主計測: `./bench/run.sh tls-upload-unary` — リクエスト payload sweep(デフォルト 1KB〜4MB)。`upload_unary_262144b` / `1048576b` / `4194304b` の `wall_time_ns_per_call` で `SSL_write` 集約の効果を見る。
- 補助: `GRPC_LITE_TRACE_FILE` 有効で 1 RPC あたりの `wire.tls_write` 件数を before/after 記録(1MB 送信で 約64回 → 約16回 が期待値)。
- ini 構成の確認: `BENCH_PHP_EXTRA_INI_ARGS="-d grpc_lite.http2_max_frame_size=65536" ./bench/run.sh tls-upload-unary` で max frame size 拡大時も coalescing が効くこと。
- 回帰確認: `./bench/run.sh tls-cpu-micro`(小 RPC でバッファ拡大による悪化なし)。

## Progress

- 2026-06-12: 実装完了。`GRPC_LITE_H2_WRITE_COALESCE_CAPACITY`(16384 固定)を廃止し、接続作成時に `h2_write_coalesce_capacity_for_max_frame_size()` で `write_buffer_cap = clamp(4 * (max_frame_size + 9), 64KB, 1MB)` を設定。`h2_connection_buffer_or_write` のバイパス判定・確保サイズを `connection->write_buffer_cap` 参照に変更。上限 1MB は `grpc_lite.http2_max_frame_size` を 16MB に設定しても persistent 接続あたりのメモリが暴れないためのガード(cap を超えるフレームは従来どおり直接 write)。

## Verification

- PHPT 15/15、C unit、静的解析、PHPUnit 30/30 すべて PASS。
- trace I/O(`trace-io-probe.sh 10 1048576`、10 RPC 合計): TLS 送信 `wire.tls_write` **685 → 196**(約 68.5 → 19.6 回/RPC、約 1/3.5)。
- ベンチ(before: `main-baseline-20260612` / after: `coalesce-cap-after-20260612` + 再計測 2 回):
  - tls-upload-unary p50 1MB: 869.2 → {845.9, 878.0, 905.1}µs、4MB: 3460.6 → {3549.1, 3397.4, 3712.3}µs — **wall は揺れ幅内で改善・悪化なし**。SSL_write の per-call 固定費は loopback では暗号化(バイト比例)に対して小さく、record オーバーヘッド削減(~0.2%)は観測限界以下。
  - 256KB: 236.9 → 222.0µs、64KB: 80.7 → 76.5µs(微改善、揺れ幅内)。
  - `BENCH_PHP_EXTRA_INI_ARGS="-d grpc_lite.http2_max_frame_size=65536"` 構成: 4MB p50 3046.3µs — mfs 拡大時も coalescing が機能(従来は完全無効化されていた)。
  - 回帰: tls-cpu-micro tiny_unary_0b 12.4µs cpu / 34.6µs wall(悪化なし)。

## Decision Log

- 2026-06-12: **採用**。loopback ベンチでは wall の改善は観測できないが、(1) `SSL_write` 回数 1/3.5 の削減は実ネットワーク(レイテンシ・per-record コストが効く環境)で効く方向、(2) ini で max_frame_size を 16KB 超に設定すると coalescing が事実上無効化される潜在バグの解消、(3) #6 no-copy-send が前提とする coalesce 構造の確定、を理由とする。メモリ増は persistent 接続あたり +48KB(デフォルト構成、64KB cap)× 上限 128 接続 = 最大 8MB で許容範囲。
