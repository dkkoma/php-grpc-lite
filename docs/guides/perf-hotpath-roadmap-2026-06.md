# C Core / Zend 境界 性能最適化ロードマップ (2026-06)

2026-06-12 の C Core + Zend 境界コードレビュー(HTTP/2 RFC 9113 / gRPC over HTTP/2 spec 照合込み)で起票した最適化 issue 群の進め方ガイド。各 issue の詳細・修正方法・完了条件は `docs/issues/open/` の個別ファイルを一次ソースとし、本書は順序・依存関係・共通検証プロトコルだけを定める。

このロードマップ用にベンチ suite を拡張済み(2026-06-12):

- `payload-unary` のレスポンス sweep デフォルトを 0〜4MB に拡大(従来は最大 100KB)。
- `tls-payload-unary` を新設(TLS 経路の受信 sweep。SSL_read 削減系の主計測)。
- `upload-unary` / `tls-upload-unary` を新設(**リクエスト** payload sweep 1KB〜4MB。従来は送信方向のサイズ sweep が存在しなかった)。test-server の `BenchRequest.request_payload` を利用。
- いずれも `tools/benchmark/payload-unary.php` に統合(suite 名で方向と TLS を切り替え。TLS suite は target が `test-server:50052` に自動切替)。

## 対象 issue 一覧

| # | Issue | 種別 | 期待効果 | 実装コスト | リスク |
|---|---|---|---|---|---|
| 1 | [trace-getenv-hotpath-caching](../issues/open/2026-06-12-trace-getenv-hotpath-caching.md) | CPU固定費 | 毎フレーム/毎I/Oの `getenv()` 排除 | 小 | 低 |
| 2 | [unary-response-direct-decode](../issues/open/2026-06-12-unary-response-direct-decode.md) | コピー削減 | 受信 memcpy 約半減 + realloc 排除 + framing 1回化 | 中 | 中 |
| 3 | [unary-recv-buffer-size](../issues/open/2026-06-12-unary-recv-buffer-size.md) | syscall削減 | 大レスポンスの read ループ約1/4 | 小〜中 | 低 |
| 4 | [write-coalesce-capacity-vs-frame-size](../issues/open/2026-06-12-write-coalesce-capacity-vs-frame-size.md) | syscall/TLS record削減 | 大リクエストの `SSL_write` 約1/4 | 小 | 低 |
| 5 | [h2c-write-coalescing](../issues/open/2026-06-12-h2c-write-coalescing.md) | syscall削減 (平文) | 平文送信の `send()` 集約 | 小 | 低 |
| 6 | [request-data-no-copy-send](../issues/open/2026-06-12-request-data-no-copy-send.md) | コピー削減 | 送信 memcpy 半減 (`NGHTTP2_DATA_FLAG_NO_COPY`) | 中〜大 | 中 |
| 7 | [status-headers-in-metadata-map](../issues/open/2026-06-12-status-headers-in-metadata-map.md) | 互換性 + alloc削減 | ext-grpc 互換の metadata 露出 + 毎RPC数allocs削減 | 小 | 中(挙動変更) |

## 依存関係

```
#1 trace-getenv ──────────────── 独立。最初にやる(後続ベンチのノイズ削減にもなる)
#3 recv-buffer ───────────────── 独立。ただし #2 と同じ受信ループを触る
#2 unary-direct-decode ───────── #3 と同時期に入れると計測が分離できない → 順番に
#4 coalesce-capacity ─────────── 独立(送信側)
#5 h2c-coalescing ────────────── #4 と同じ write 経路。#4 の後が楽
#6 no-copy-send ──────────────── #4/#5 の coalesce 構造が確定してから(send_data_callback が
                                  h2_connection_buffer_or_write を呼ぶため)
#7 status-headers ────────────── 独立。性能ではなく挙動変更なので単独PRで出す
```

## 推奨フェーズ

### Phase 0: ベースライン確定

- main で以下を実行し、run id (`BENCH_TAG`) を記録:
  - `./bench/run.sh spanner-shape`(現実的 shape の回帰基準)
  - `./bench/run.sh cpu-micro` / `tls-cpu-micro`(固定費基準)
  - `./bench/run.sh payload-unary` / `tls-payload-unary`(受信 payload sweep、0〜4MB)
  - `./bench/run.sh upload-unary` / `tls-upload-unary`(送信 payload sweep、1KB〜4MB)
- `GRPC_LITE_TRACE_FILE` で `wire.tls_read` / `wire.tls_write` / `wire.socket_write` 件数を 1 RPC あたりで記録しておく(syscall 削減系 #3/#4/#5 の before/after に使う)。

#### Phase 0 実施記録 (2026-06-12)

- run id: `BENCH_TAG=main-baseline-20260612`(7 suite すべて同一 run id で otelop に保存済み)。
- 注意: upload-unary / tls-upload-unary の 4MB は test-server の gRPC default 受信上限 (4MB) に当たって失敗したため、test-server に `grpc.MaxRecvMsgSize(64MB)` を入れて再計測した(コミット済み)。**以後の before/after もこの test-server 構成で揃えること。**
- 代表値 (span_p50_us): cpu-micro tiny_unary_0b=31.4 / small_unary_100b=27.7、tls-cpu-micro tiny_unary_0b=34.8 / small_unary_100b=32.7、payload-unary 1MB=554.4 / 4MB=2233.2、tls-payload-unary 1MB=800.1 / 4MB=3248.2、upload-unary 1MB=444.3 / 4MB=2010.2、tls-upload-unary 1MB=869.2 / 4MB=3460.6。
- trace I/O 件数 (`./tools/benchmark/trace-io-probe.sh 10 1048576`、10 RPC 合計、1MB):
  - 受信: `wire.socket_read`=662(平文) / `wire.tls_read`=666(TLS) → 約 66 read/RPC
  - 送信: `wire.socket_write`=692(平文) / `wire.tls_write`=685(TLS) → 約 69 write/RPC

### Phase 1: 低リスク・独立(まとめて進めて良い)

1. **#1 trace-getenv** — 純粋な固定費削減。後続フェーズの計測ノイズも減るため最初。
2. **#7 status-headers** — 性能とは独立した挙動変更なので、性能 PR と混ぜず単独でレビュー・マージ。GAX の `grpc-status-details-bin` 参照経路の統合テストを必ず通す。

### Phase 2: 受信経路

3. **#3 recv-buffer** — バッファ拡大のみ。trace で read 回数削減を確認してから #2 へ。
4. **#2 unary-direct-decode** — 本ロードマップ最大の本命。direct decode 経路は streaming で実戦投入済みだが、unary 固有のエッジ(trailers-only、空 message、truncated body、`max_response_messages=1` 超過)を PHPT で固める。open issue [response-delivery-hotpath](../issues/open/2026-05-14-response-delivery-hotpath.md) のスコープと重なるため、そちらの進捗・計測結果を先に確認し、必要なら統合する。

### Phase 3: 送信経路

5. **#4 coalesce-capacity** — 定数変更 + cap 参照化。大 payload 送信ベンチで `SSL_write` 回数削減を確認。
6. **#5 h2c-coalescing** — フラグ1行 + メモリ増の確認。平文は本番経路でない場合、優先度を下げて良い(emulator/CI 高速化が主目的)。
7. **#6 no-copy-send** — 送信経路の構造変更。#4/#5 確定後に着手。フレーム境界・5 byte prefix またぎ・空 payload の PHPT を先に書いてから実装する(テストファースト推奨)。

## 共通検証プロトコル(全 issue 共通ゲート)

各 issue のブランチで、マージ前に以下を通す。

1. **テスト**
   - PHPT: `./tools/test/check-phpt.sh`(Go test-server ports 50051–50060 preflight)
   - C unit: `./tools/test/check-c-unit.sh`
   - 静的解析: `./tools/test/check-c-static-analysis.sh`
   - 統合: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist`
2. **HTTP/2/gRPC ドメインモデルレビュー**(AGENTS.md 必須ゲート)
   - transport / protocol に触る #2〜#6 は `docs/verification/protocol-model-review-guide.md` に沿って Blocker/High/Medium/Low が none になるまで実施。
   - #1/#7 は診断・metadata 露出のみだが、#7 は metadata/status 責務に触るためレビュー対象。
3. **ベンチ**
   - 各 issue の「測定ベンチマーク」節に指定された suite を before/after 同条件で実行し、OTEL span 集計 (`tools/benchmark/otelop-summary.php`) で比較。issue ごとの主計測 suite は下表のとおり。

     | Issue | 主計測 | 回帰確認 |
     |---|---|---|
     | #1 trace-getenv | cpu-micro, tls-cpu-micro | spanner-shape |
     | #2 unary-direct-decode | payload-unary, tls-payload-unary (1MB/4MB) | spanner-shape, metadata-header |
     | #3 recv-buffer | tls-payload-unary (1MB/4MB) + trace read 件数 | cpu-micro, large-streaming |
     | #4 coalesce-capacity | tls-upload-unary (256KB〜4MB) + trace write 件数 | tls-cpu-micro |
     | #5 h2c-coalescing | cpu-micro, upload-unary | tls-cpu-micro, spanner-shape |
     | #6 no-copy-send | upload-unary, tls-upload-unary (1MB/4MB) | cpu-micro, tls-cpu-micro, spanner-shape |
     | #7 status-headers | (性能計測不要) | metadata-header, spanner-shape |

   - 採否判断は上記制御ベンチ + spanner-shape を主、`spanner-real-client` は悪化監視として扱う(emulator の揺れが大きいため。metadata-conversion-hotpath issue の前例に従う)。
   - 悪化が見えたら採用見送りも選択肢。**見送り判断も issue の Decision Log に数値付きで残す**(native-h2-write-coalescing の前例)。
4. **issue 更新**
   - 進捗・計測値・判断を各 issue の Progress / Verification / Decision Log に追記。
   - 完了時は Status: Closed にして `docs/issues/closed/` へ移動。

## PR 分割方針

- 1 issue = 1 ブランチ = 1 PR。性能変更と挙動変更(#7)は混ぜない。
- #2 は変更量が大きくなるため、「unary を direct decode に切り替える」「extract_unary_payload 削除 + 所有権移動」の 2 コミットに分けると review しやすい。
- #6 は「callback 配線 + フォールバック」「trace 追従」を分ける。

## 進捗トラッキング

| Issue | Status | Branch / PR | 計測 | 判断 |
|---|---|---|---|---|
| #1 trace-getenv | PR review待ち | perf/trace-getenv-hotpath-caching / [#15](https://github.com/dkkoma/php-grpc-lite/pull/15) | 改善はノイズ床以下 | 採用(hot path 衛生) |
| #7 status-headers | PR review待ち | fix/status-headers-in-metadata-map / [#16](https://github.com/dkkoma/php-grpc-lite/pull/16) | metadata-header/spanner-shape 悪化なし | 採用(ext-grpc 互換) |
| #3 recv-buffer | PR review待ち | perf/unary-recv-buffer-size / [#17](https://github.com/dkkoma/php-grpc-lite/pull/17) | 平文 read 66→17.7/RPC、平文1MB -7〜10%、TLS は SSL_read 仕様上不変 | 採用 |
| #2 unary-direct-decode | PR review待ち (#17 に stacked) | perf/unary-response-direct-decode / [#18](https://github.com/dkkoma/php-grpc-lite/pull/18) | **4MB 受信 p50 平文 -33% / TLS -27%** | 採用(本命) |
| #4 coalesce-capacity | PR review待ち | perf/write-coalesce-capacity / [#19](https://github.com/dkkoma/php-grpc-lite/pull/19) | SSL_write 68.5→19.6/RPC、wall は揺れ幅内 | 採用(潜在バグ解消 + #6 前提) |
| #5 h2c-coalescing | PR review待ち (#19 に stacked) | perf/h2c-write-coalescing / [#20](https://github.com/dkkoma/php-grpc-lite/pull/20) | 平文 send() 69→19/RPC | 採用 |
| #6 no-copy-send | PR review待ち (#20 に stacked) | perf/request-data-no-copy-send / [#21](https://github.com/dkkoma/php-grpc-lite/pull/21) | 平文 1MB 送信 -7%、TLS 4MB 改善側 | 採用 |

- マージ順: #15 / #16 / #19 は独立。#17 → #18、#19 → #20 → #21 は stacked(マージごとに次の PR の base を付け替え)。
- 各 issue の詳細計測・Decision Log は issue ファイル、ドメインモデルレビューは `docs/reviews/issues/2026-06-12-*.md` を参照。マージ時に issue を Status: Closed にして `docs/issues/closed/` へ移動する。
- 副産物: #2 で [response-delivery-hotpath](../issues/open/2026-05-14-response-delivery-hotpath.md) の unary 側スコープを実装済み。#3 で「TLS の read 回数はアプリバッファ拡大では減らない(SSL_read は 1 record/call)」という知見を記録。

## レビューで「問題なし」と確認済みの領域(再調査不要)

- gRPC 5-byte prefix 検証(compressed flag / 長さ / message 数上限)、grpc-status 値域、`grpc-timeout` フォーマット(8桁上限・単位繰り上げ)は spec 準拠。
- SETTINGS(ENABLE_PUSH=0 / INITIAL_WINDOW_SIZE 8MB / connection WINDOW_UPDATE)、GOAWAY 後の REFUSED_STREAM 処理は RFC 9113 準拠。
- フロー制御は nghttp2 auto window update + 8MB デフォルトウィンドウでスループット律速になっていない。
- metadata 変換の HashTable コピー削減は実施済み([closed issue](../issues/closed/2026-05-14-metadata-conversion-hotpath.md))。request header の heap allocation 削減も実施済み(inline 配列化)。
