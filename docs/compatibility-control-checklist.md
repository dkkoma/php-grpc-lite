# gRPC 互換性・制御系チェックリスト

この文書は性能ベンチマークではなく、gRPC ライブラリとして落としてはいけない semantics / control path を実装時に確認するためのチェックリスト。性能差として追う項目は `docs/benchmarks/README.md` に置く。

## 1. Scope

現時点の主対象は unary と server streaming。client streaming / bidi streaming は API stub として存在しても、実装対象フェーズが来るまで別扱いにする。

## 2. Client Deadline / Timeout

2026-04-27 時点で、unary / server streaming の client-side enforcement と `STATUS_DEADLINE_EXCEEDED` 変換は実装済み。`grpc-timeout` header の 8 桁制限や unit 丸めの厳密な ext-grpc 互換は引き続き確認対象。

| 項目 | 期待 | テスト観点 |
|---|---|---|
| `timeout` option → `grpc-timeout` | gax が渡す microseconds timeout を仕様の `grpc-timeout` header に変換する | header 値、unit 丸め、8桁制限 |
| client-side enforcement | サーバが遅い/応答しない場合も client 側で待ち続けない | `CURLOPT_TIMEOUT_MS` 等で elapsed が timeout 付近に収まる |
| status mapping | client-side timeout は `STATUS_DEADLINE_EXCEEDED` として返る | unary と server streaming の両方 |
| resource cleanup | deadline 超過後の handle は安全に破棄される | 次の warm RPC が壊れない。破棄/再利用の判断を明示 |

## 3. Cancellation / Early Close

| 項目 | 期待 | テスト観点 |
|---|---|---|
| explicit `cancel()` | 呼び出し中の RPC を中断し、以後 response を読めない | unary 開始後、server streaming 中 |
| streaming early break | consumer が途中で iteration を止めても接続状態を汚さない | N 件だけ読んで break。次の RPC が成功する |
| handle reuse policy | 完走していない stream の handle は原則再利用しない | reuse pool に戻らないことを確認 |
| status visibility | cancellation 後の status の扱いが ext-grpc と大きくズレない | 例外/Status object/metadata の形を確認 |

## 4. Response Status / Error Semantics

2026-04-28 時点で、unary / server streaming の trailers-only `grpc-status` 分類、`grpc-message` percent decode、HTTP status / content-type validation は実装済み。

| 項目 | 期待 | テスト観点 |
|---|---|---|
| trailers-only response | body なしで `grpc-status` が header block に来ても status として扱う | unary immediate error、server streaming immediate error |
| `grpc-message` percent decode | percent-encoded UTF-8 を decode し、不正 encoding でも壊れない | `%20`、UTF-8、壊れた `%` |
| missing trailers | trailers が欠落した場合は適切な non-OK status を合成する | `STATUS_UNKNOWN` + details の妥当性 |
| HTTP non-200 | gRPC status が無い HTTP error を gRPC status に合成する | 404/503/502 等 |
| content-type mismatch | `application/grpc` でない応答を成功扱いしない | proxy/html/json response |
| `grpc-status-details-bin` | status details がある場合に矛盾を検出する | status code mismatch はエラー扱い |

## 5. Metadata Compatibility

| 項目 | 期待 | テスト観点 |
|---|---|---|
| ASCII metadata | key/value 正規化が gRPC 仕様と ext-grpc に沿う | lowercase key、重複 key、空白 |
| binary metadata | `*-bin` は base64 decode/encode 互換を守る | padded/unpadded、`,` join の split |
| reserved `grpc-*` | application metadata として不正な reserved key を混ぜない | request metadata validation |
| initial/trailing split | body 前後で metadata を正しく分離する | normal response、trailers-only |
| metadata size | 過大 metadata の扱いを決める | 明示エラーか libcurl/server に委ねるか |

2026-04-28 時点で、単一 raw binary value の `*-bin` request / initial / trailing metadata round-trip は php-grpc-lite と ext-grpc で一致確認済み。同一 key の複数 binary values は ext-grpc 側で最後だけ見える挙動が観測されたため、追加確認対象として残す。

## 6. Compression / Encoding

2026-04-28 時点で、未対応の `grpc-encoding` と compressed flag=1 は `STATUS_UNIMPLEMENTED` として明示エラー化済み。実際の gzip 対応は未実装。

| 項目 | 期待 | テスト観点 |
|---|---|---|
| unsupported response encoding | 未対応の `grpc-encoding` や compressed flag=1 を成功 decode しない | 明示的な non-OK / exception 方針 |
| per-message compression | 対応後は message ごとに独立して decompress する | streaming で複数 compressed message |
| request encoding | 対応する場合は `grpc-encoding` と compressed flag を揃える | unary request、server streaming request |

## 7. HTTP/2 Connection Control

| 項目 | 期待 | テスト観点 |
|---|---|---|
| connection failure | 検出可能な接続失敗を `STATUS_UNAVAILABLE` 系にする | server restart、connection close |
| GOAWAY | 新規 stream が拒否された場合に壊れた handle を再利用しない | 可能なら h2 test server で再現 |
| RST_STREAM | stream reset を適切な status に変換する | CANCEL、REFUSED_STREAM、INTERNAL_ERROR |
| keepalive / idle reuse | idle 後の再利用が失敗したら破棄して次 call に影響させない | 長時間 idle 後の warm RPC |

## 8. Streaming Flow Control / Consumer Behavior

| 項目 | 期待 | テスト観点 |
|---|---|---|
| slow consumer | consumer が遅い場合も memory が無制限に膨らまない | 性能 bench 側では peak memory も測る |
| large message stream | 大きい message が分割されても frame を再構成できる | 1 frame が複数 write callback に分割 |
| coalesced messages | 1 chunk に複数 gRPC frame が入っても全て yield する | count=1000 等 |
| ordering | stream 内 message order を維持する | sequence number payload で確認 |

## 9. TLS / Authority / User-Agent

| 項目 | 期待 | テスト観点 |
|---|---|---|
| TLS verification | root cert / SNI / authority が正しく効く | 正常 CA、不正 CA、hostname mismatch |
| mTLS | client cert/key が正しく送られる | CA blob / temp file fallback |
| authority override | channel option / metadata と URL host の扱いを決める | Spanner 等の実用経路 |
| user-agent | gRPC client として診断可能な UA を送る | gax の agent header と衝突しない |

## 10. Retry / Idempotency

gRPC 仕様上、call は明示されない限り idempotent ではない。ライブラリが勝手に application-level retry を入れるべきではない。

| 項目 | 期待 | テスト観点 |
|---|---|---|
| no implicit retry | transport error 時に同じ RPC を勝手に再送しない | server 側 call count |
| retryable status exposure | 上位ライブラリが retry 判断できる status を返す | `UNAVAILABLE`、`DEADLINE_EXCEEDED` |
| connection reuse retry | 壊れた reused handle の失敗を次 call に持ち越さない | retry ではなく handle discard として確認 |

## 11. Benchmark との境界

- elapsed time や memory が主目的なら `docs/benchmarks/README.md` の性能 suite に置く。
- status、metadata、resource cleanup、handle reuse policy が主目的ならこの文書の互換性/制御系テストに置く。
- 同じ scenario を両方で使う場合も、性能 bench は代表値と揺れ、互換性テストは期待 status と後続 call の安全性を別々に検証する。
