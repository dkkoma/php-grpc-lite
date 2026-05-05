# php-grpc-lite 仕様書

> **Living document.** 実装中に前提が変わるたびに本書を更新する。末尾の「変更履歴」に日付付きで追記すること。
> 仕様と実装が乖離した場合、原則として **仕様書を先に更新してから実装を進める**。

---

## 1. 目的

`grpc/grpc` 公式 PHP 拡張(以下 `ext-grpc`)のドロップイン代替として、**SEGV を構造的に抑制**した gRPC クライアント機能を提供する PHP 拡張。

学習プロジェクトを兼ねる。

### 1.1 解決したい現状の問題

- `ext-grpc` は gRPC C-core(libgrpc)を Zend API でラップしており、以下に起因する SEGV が頻発する:
  - C-core の completion queue(非同期 poll モデル)と PHP の同期リクエスト寿命のミスマッチ
  - Persistent channel と PHP-FPM fork モデルの相性
  - C-core 側オブジェクトと zval の参照寿命のずれ
- `ext-grpc` は BoringSSL を同梱するため、PHP の `ext-openssl`(OpenSSL)と同居時にシンボル衝突・SSL_CTX 分裂が発生する。とくに PHP コアと static link する構成で問題が顕在化する。

---

## 2. 非目標(現時点)

- gRPC **サーバー** 機能の提供(別途検討)
- gRPC C-core(libgrpc)のラップ — SEGV の主因のため、**意図的に避ける**
- BoringSSL の同梱 — OpenSSL 衝突回避のため、**意図的に避ける**

---

## 3. スコープ

### 3.1 現スコープ

- gRPC クライアント
- Unary RPC
- Server Streaming RPC
- TLS / mTLS
- メタデータ送受信
- Deadline
- Call credentials(`createFromPlugin`、Application Default Credentials を含む)

### 3.2 後続フェーズで検討

- Client Streaming RPC
- Bidi Streaming RPC
- Channelz
- Persistent channel pool の互換動作

### 3.3 スコープ外

- gRPC サーバー実装

---

## 4. 設計戦略

### 4.1 段階的実装

| Phase | 内容 | 完了条件 | 状態 |
|---|---|---|---|
| 0 | 純 PHP PoC。libcurl + ext-curl で HTTP/2 を喋り、gRPC framing を PHP で実装。`Grpc\` 互換 API を提供 | unary と server streaming の sample が動く | **2026-04-25 完了** |
| 1 | ベンチマーク基盤整備 | レイテンシ/スループット/メモリ計測を継続比較できる | **2026-04-28 完了**。Docker ローカル実行・ログ保存・JSON/TSV 抽出・baseline check/update・ext-grpc 比較入口を整備済み |
| 2 | ホットパスの拡張化(framing, metadata, status 解釈) | ベンチで純 PHP より明確に速い | Phase 2 PoC比較により native nghttp2 transport 方向に決定。詳細は `docs/native-transport-decision.md` |
| 3 | nghttp2 直接呼び出しに置き換え | runtime transportからlibcurlを外し、native nghttp2 transport 1系統にする | 実装中。drop-in releaseにはpackaging / lifecycle / memory QAのrelease gateを置く。詳細は `docs/native-transport-design.md` / `docs/release-qa-checklist.md` |

release default native のQA判定は `docs/release-qa-checklist.md` に集約する。

各フェーズの遷移はベンチマーク結果で判断する。2026-05時点ではPhase 3の本線を native nghttp2 transport 1系統とし、libcurl runtime routeは残さない。

### 4.2 HTTP/2 トランスポート

runtime transportは native nghttp2 + 自前socket/TLS の1系統とする。PHP userlandの `Grpc\` surfaceはtransport選択optionを持たず、native extension未ロードやtransport errorは別経路へfallbackしない。

#### Call 種別ごとのtransport利用パターン

| Call 種別 | 関数 | 理由 |
|---|---|---|
| Unary | native persistent channel + 1 HTTP/2 stream | 単発往復。C側でHTTP/2 session/socketをprocess-local / thread-localに再利用する |
| Server streaming | native stream resource + Generator pull | message単位でyieldし、slow consumer時はread/WINDOW_UPDATE進行を抑える |
| Client streaming(後回し) | 未実装 | 後続フェーズで設計 |
| Bidi streaming(後回し) | 未実装 | 後続フェーズで設計 |

#### 重要な前提

- gRPC は status を **HTTP/2 trailers**(`grpc-status`, `grpc-message`)で返す。
- gRPC framing は `1 byte compressed-flag + 4 byte big-endian length + payload`。
- TLSではALPNで `h2` を交渉する。h2c(plaintext)ではHTTP/2 prior knowledgeで接続する。
- 圧縮messageは圧縮実装が入るまで `STATUS_UNIMPLEMENTED` とする。

### 4.3 TLS 戦略

- システム OpenSSL を使用する。PHP の `ext-openssl` と同じ libssl を共有する。
- BoringSSL は同梱しない。
- native transportは nghttp2 + OpenSSL を直接使う。
- mTLS(client cert)、CallCredentials によるトークン付与は OpenSSL の機能で完全に実現可能。
- **PEM の渡し方**: `ChannelCredentials` のroot cert / client cert / private keyをC extensionへ渡し、OpenSSLの `SSL_CTX` に設定する。
- **実機検証済**: Trixie の OpenSSL 3.5 で h2 ALPN ネゴシエーション、自己署名 CA 経由の cert verification、cert mismatch 時の `STATUS_UNAVAILABLE` エラー伝播を確認。

### 4.4 Protobuf

- `protobuf` PECL 拡張(`pecl install protobuf`)を前提とする。
- メッセージ型は `Google\Protobuf\Internal\Message` 互換のものを扱う。
- スタブコード生成は既存の `protoc-gen-php-grpc` 互換出力を当面そのまま受け入れる(独自コードジェネレータは後続検討)。

### 4.5 API 互換性

`google/gax` および `google/cloud-*` が利用する `Grpc\` 名前空間 API を網羅する。
詳細な呼び出し仕様は [`docs/api-surface.md`](./api-surface.md) を参照(2026-04-25 にソース調査で裏取り済み)。

#### 拡張モジュール名(重要)

本拡張は **`grpc`** という名前で登録する。`gax-php` が `extension_loaded('grpc')` でサポート可否を判定するため。これにより `ext-grpc` と同居不可になるが、ドロップイン代替の定義そのものなので妥当。

#### 互換シンボル分類(調査結果反映後)

| 種別 | シンボル | 互換必須度 | 備考 |
|---|---|---|---|
| Stub 基底 | `Grpc\BaseStub` | 必須 | 4 protected メソッド: `_simpleRequest`, `_serverStreamRequest`, `_clientStreamRequest`, `_bidiRequest`(後 2 つは Phase 0 では未対応エラーで可) |
| Channel | `Grpc\Channel` | 必須 | `__construct(string $hostname, array $opts)` |
| Channel | `Grpc\ChannelCredentials` | 必須 | static `createSsl`, `createInsecure`, `createDefault` |
| Call | `Grpc\AbstractCall` | 必須 | 共通親クラス |
| Call | `Grpc\UnaryCall` | 必須 | `wait()`, `cancel()`, `getMetadata()`, `getTrailingMetadata()` |
| Call | `Grpc\ServerStreamingCall` | 必須 | `responses()`, `getStatus()`, `getMetadata()`, `getTrailingMetadata()`, `cancel()` |
| Call | `Grpc\ClientStreamingCall` | 後回し | スコープ外。BaseStub のスタブだけ用意し未対応エラーを投げる |
| Call | `Grpc\BidiStreamingCall` | 後回し | 同上 |
| Interceptor | `Grpc\Interceptor` | 必須 | static `intercept()` + 派生クラスのフック 4 種 |
| Status | `Grpc\STATUS_*` 定数 17 個 | 必須 | 値は 0–16(`google.rpc.Code` と同じ) |
| 時刻 | `Grpc\Timeval` | 必須(薄実装) | gax は使わないが、レガシーユーザーコード互換のため `__construct(int $microseconds)` / `infFuture()` / `infPast()` / `now()` / `microtime()` を提供 |
| 内部 | `Grpc\CallInvoker` | 観測振る舞いのみ | grpc-gcp 連携時のみ実体が必要 |
| 範囲外 | `Grpc\Gcp\*` | — | 別 Composer パッケージ `grpc/grpc-gcp` の責務 |

#### Status オブジェクトの形

`UnaryCall::wait()` の第 2 戻り値、および `ServerStreamingCall::getStatus()` の返り値は **stdClass 互換** で以下のプロパティを持つ:

- `code`: `int`(`Grpc\STATUS_*` のいずれか)
- `details`: `string`(trailer の `grpc-message` をデコード)
- `metadata`: `array`(trailing metadata)

#### per-call options(`_simpleRequest` 等の第 5 引数)

- `call_credentials_callback`: `callable(string $serviceUrl, string $methodName): array<string, string>` — ADC 統合に必須
- `timeout`: `int` — マイクロ秒単位

---

## 5. 環境

### 5.1 対応 PHP バージョン

- PHP **8.4+**(NTS 主軸)
- ZTS は将来検討(現時点では非対象)

### 5.2 開発環境

- Debian **Trixie** ベースのコンテナ
- ベースイメージ: `php:8.4-cli-trixie`
- ホスト OS 非依存(macOS の LibreSSL/Homebrew には依存しない)
- 起動方法: `docker compose run --rm dev`
- 同梱ツール: build-essential, autoconf, pkg-config, libssl-dev, libnghttp2-dev, protobuf-compiler, composer, pecl protobuf 拡張

### 5.3 ビルド/配布

- 開発時: ローカルビルド(`phpize && ./configure && make`)
- 配布: 最終的に PIE(`pecl/pie`)経由

---

## 6. 未決事項

- [x] ~~`google/gax` から呼ばれる `Grpc\` API の正確な一覧化~~ → `docs/api-surface.md` で完了(2026-04-25)
- [x] ~~`Grpc\CallCredentials::createFromPlugin()` の正確な仕様確認(api-surface.md §5)~~ → callback wrapperとして実装し、per-call option `call_credentials` から metadata を生成する(2026-05-04)
- [ ] generated stub(`*GrpcClient.php`)の典型実装の確認(`protoc-gen-php-grpc` 出力例)
- [x] ~~`ServerStreamingCall::responses()` Generator の実装戦略~~ → native stream resourceをGeneratorがpullし、message単位でyieldする。slow consumer時はread/WINDOW_UPDATE進行を抑え、stream resource destructor / `cancel()` で `RST_STREAM(CANCEL)` を送る。
- [ ] テスト用 gRPC サーバーの選定(Go の helloworld を `compose.yaml` に並べる案が有力)
- [ ] ベンチマーク手法とターゲット環境(計測対象、繰り返し回数、コンパレータ)
- [ ] Persistent channel pool の互換要件(ext-grpc は `grpc.use_local_subchannel_pool` 等の INI で制御)
- [ ] マルチアーキテクチャ対応(arm64 / amd64)
- [ ] エラーログ/デバッグ出力の方式
- [x] ~~trailers-only error response の扱い(`grpc-status` が body 前 header block に来るケース)~~ → body 前でも `grpc-status` / `grpc-message` / `grpc-status-details-bin` は trailing status metadata として扱う(2026-04-27)
- [x] ~~`grpc-message` の percent decode~~ → status details へ入れる前に `rawurldecode()` する(2026-04-27)
- [x] ~~HTTP status / `content-type: application/grpc` validation~~ → `grpc-status` が無い非 gRPC 応答は HTTP status から gRPC status を合成し、HTTP 200 でも `content-type` が `application/grpc` でなければ `STATUS_UNKNOWN` とする(2026-04-28)
- [x] ~~client-side deadline enforcement(gax の `timeout` option を `grpc-timeout` header だけでなくtransport I/Oにも反映し、クライアント側でも `DEADLINE_EXCEEDED` を保証する)~~ → unary / server streaming ともにRPC deadlineをconnect / TLS handshake / read-write poll loopの上限として扱い、deadline超過を `STATUS_DEADLINE_EXCEEDED` に変換する。`grpc-timeout` は8桁制限に収まるよう `u` / `m` / `S` / `M` / `H` へ単位変換する(2026-05-04)
- [x] ~~圧縮(`grpc-encoding`, compressed flag=1)の扱い。未対応なら明示エラー化~~ → 未対応の `grpc-encoding` と compressed flag=1 は `STATUS_UNIMPLEMENTED` にする(2026-04-28)
- [x] ~~binary metadata(`*-bin`)の ext-grpc 互換確認~~ → PHP API の値は raw binary、HTTP/2 wire は base64 として扱う。単一 raw binary value の request/initial/trailing round-trip を ext-grpc と照合(2026-04-28)
- [x] ~~binary metadata の同一 key 複数 value における ext-grpc 互換確認~~ → 公式 ext-grpc PHP API は最後 value のみ可視だが、php-grpc-lite は gRPC 仕様準拠を優先し、同一 key 複数 values を `array<string, list<string>>` として保持する(2026-05-04)

### 6.1 Metadata shape policy

gRPC metadata は同一 key に複数 values を持てる。php-grpc-lite の PHP API では request / initial / trailing / status metadata を `array<string, list<string>>` として扱い、同一 key 複数 values の順序と内容を保持する。`*-bin` metadata は PHP API 上 raw binary values、HTTP/2 wire 上 base64 values とし、response 側で comma-joined binary metadata を受けた場合も split して複数 raw binary values として返す。

2026-05-04 の観測では、公式 ext-grpc PHP API は同一 key 複数 values の最後 value のみを返した。これは gRPC Core / HTTP/2 metadata semantics ではなく PHP extension surface の情報落ちとして扱う。drop-in 互換のために php-grpc-lite 側で metadata values を最後値へ畳む処理は入れない。

### 6.2 Request metadata control policy

request metadata は transport へ渡す前に共通正規化する。key は lowercase に寄せ、HTTP/2 pseudo header(`:*`)・space入り key・non-ASCII key・HTTP/2/gRPC metadata key範囲外の key は `InvalidArgumentException` で拒否する。ASCII metadata value は CR/LF と non-ASCII を拒否する。`*-bin` metadata value は PHP API 上 raw binary として受け付け、wire では base64 化する。

`content-type`、`te`、`user-agent`、`grpc-status`、`grpc-message`、`grpc-status-details-bin`、`grpc-timeout`、`grpc-encoding`、`grpc-accept-encoding` は library-owned metadata とし、user metadata からは送信しない。`grpc-timeout` は call option `timeout` からのみ生成し、`user-agent` は channel option `grpc.primary_user_agent` からのみ生成する。

互換性・制御系の実装時チェックリストは `docs/compatibility-control-checklist.md` に集約する。性能ベンチに入れる項目は `docs/benchmarks/README.md` に置き、deadline/error/cancellation などの semantics 検証と混ぜない。

---

## 変更履歴

- **2026-04-25**: 初版作成。目的・スコープ・段階戦略・TLS/HTTP/2 方針・API 互換目標・開発環境を確定。
- **2026-04-25**: Dockerfile および compose.yaml を追加。開発環境セクションに起動方法と同梱ツールを追記。
- **2026-04-25**: API サーフェス調査(`docs/api-surface.md`)を実施し §4.5 を更新。主な確定事項: 拡張モジュール名は `grpc`(gax の `extension_loaded('grpc')` チェックのため)、`Grpc\Gcp\*` は別パッケージで範囲外、Status オブジェクトの正確な形(`code`/`details`/`metadata`)を確定。
- **2026-04-25**: `Grpc\Timeval` をレガシーユーザーコード互換のため Phase 0 から薄く実装する方針に変更(API サーフェス §2.3 の補足参照)。
- **2026-04-25**: PoC スパイク完了(`poc/`)。unary・server streaming ともに libcurl + HTTP/2 prior knowledge で疎通成功。`§4.2 重要な前提`を実機検証結果で更新し、`responses()` Generator の実装戦略を未決事項に追加。
- **2026-04-25**: server streaming Generator 戦略を `curl_multi_*` ベースに決定(`poc/client/server_streaming_multi.php` で incremental yield を実機検証)。§4.2 に Call 種別ごとの libcurl 利用パターン表を追加。
- **2026-04-25**: **Phase 0 完了**。`src/Grpc/` 配下に純 PHP 実装を配置: `BaseStub` / `Channel` / `ChannelCredentials` / `AbstractCall` / `UnaryCall` / `ServerStreamingCall` / `Interceptor` / `InterceptorChannel` / `Timeval` / `STATUS_*` 17 定数 / `ClientStreamingCall`(stub)/ `BidiStreamingCall`(stub)。統合テスト 5 件すべてパス(unary 2 + server streaming 2 + interceptor chain 1)。Interceptor の chain order(outer:before → inner:before → 実行 → inner:after → outer:after)も検証済み。
- **2026-04-25**: TLS 経路を実機検証。test-server に h2-tls listener (port 50052) を追加、自己署名 CA を `poc/test-server/certs/` に配置。`AbstractCall::applyTlsOptions()` で `CURLOPT_CAINFO_BLOB` / `CURLOPT_SSLCERT_BLOB` / `CURLOPT_SSLKEY_BLOB` 優先 + temp file fallback。統合テストは計 8 件(TLS 系 3 件: 正常系 unary、正常系 server streaming、不正 CA で `STATUS_UNAVAILABLE` 拒否)。
- **2026-04-26**: Spanner emulator (`gcr.io/cloud-spanner-emulator/emulator`) を compose に追加し、本物の `google/cloud-spanner` 生成 protobuf 型と我々の BaseStub の組み合わせで `ListInstances` unary を実機検証。新しい `google/cloud-spanner` v2.x は GAPIC 専用で direct gRPC stub を出さないため、Phase 0 では手書きの `*GrpcClient` fixture(`tests/Integration/Fixtures/Spanner/`)を経由。ext-grpc 要件は `composer.json` の `provide.ext-grpc` + `config.platform.ext-grpc` の二段で吸収。
- **2026-04-26**: PHP 8.4 を最低バージョンに引き上げ(composer + Dockerfile)。
- **2026-04-26**: Spanner emulator 検証 Steps 2-5 完了。`CreateInstance`(LRO)、`CreateDatabase`(DDL 付き LRO)、`CreateSession` + `ExecuteSql`(unary `SELECT 1`)、**`ExecuteStreamingSql`**(server streaming、UNNEST(GENERATE_ARRAY(1,1000)) で 1000 行受信)を全て実機検証。これで「自分の helloworld を超えて、本物の googleapis 生成 protobuf 型 × 本物の C++ Spanner 実装 × 我々の BaseStub」の全層が unary + server streaming で動くことが確定。テスト合計 14 件。
- **2026-04-26**: ベンチマーク基盤と ext-grpc 比較環境を整備。`bench/`(PHPBench 1.6, シナリオ群)、`Dockerfile.ext-grpc` + `bench-comparison/`(独立 Composer プロジェクト、`grpc/grpc` 経由)、`bench/compare.sh`(両環境計測の自動化)。Spanner emulator は内部状態で run 間変動が大きいためベンチ対象から外し、Go test-server に `BenchUnary` / `BenchServerStream` を追加して payload bytes、message count、server delay を制御可能にした。安定指標では ext-grpc が軽量 unary で ~3× 速い一方、server streaming 1000 件では php-grpc-lite が ~1.8× 速い。Phase 1 の最初の観測対象が「接続プーリング(libcurl handle 再利用)で unary 固定費がどれだけ下がるか」に定まった。詳細: `docs/benchmarks/baseline-2026-04-26.md`、`docs/benchmarks/comparison-ext-grpc-2026-04-26.md`。
- **2026-04-26**: mTLS 経路を実機検証。test-server に mTLS listener (port 50053, `RequireAndVerifyClientCert`) を追加、自己署名 client cert を `poc/test-server/certs/client.{crt,key}` に配置。`ChannelCredentials::createSsl($rootCerts, $privateKey, $certChain)` の `certChain` / `privateKey` 経路(`CURLOPT_SSLCERT_BLOB` / `CURLOPT_SSLKEY_BLOB`)が実際に動くことを確認(統合テスト 2 件: client cert 付きで成功、無しで `STATUS_UNAVAILABLE` 拒否)。テスト合計 16 件。
- **2026-04-27**: Channel-scoped curl handle reuse を実装。`Channel` が idle easy handle を保持し、unary / server streaming の正常完了時に `curl_reset()` して再利用する。curl error、`cancel()`、server streaming の途中終了は接続状態が不明なため `curl_close()` で破棄する。Go test-server bench では軽量 unary が約 228 μs → 38 μs に下がり、ext-grpc の同条件 68 μs を下回った。server delay 10ms の unary は php-grpc-lite 11.6 ms / ext-grpc 11.8 ms でほぼ同等。一方、client/channel を call ごとに構築して PHP object を破棄する cold 近似では php-grpc-lite 277 μs / ext-grpc 89.7 μs で、PHP-FPM request をまたぐ persistent pool を持てる ext-grpc 側の優位が残る。詳細: `docs/benchmarks/connection-reuse-2026-04-27.md`。
- **2026-04-27**: Rust 製 drop-in 代替候補 `BSN4/grpc-php-rs` を one-shot 比較。`Dockerfile.grpc-php-rs` / `dev-grpc-rs` / `bench/compare-rs.sh` を追加し、必要時に同じ PHPBench シナリオを再実行できるようにした。通常の継続比較は `php-grpc-lite` vs 公式 `ext-grpc` の `./bench/compare.sh` のまま維持する。今回 run では `grpc-php-rs` は unary 100KB payload で最速、小さい unary は `php-grpc-lite` が最速、cold 近似と delay 系は公式 `ext-grpc` が優位。詳細: `docs/benchmarks/comparison-grpc-php-rs-2026-04-27.md`。
- **2026-04-27**: ホットパス分解スクリプトを追加し、通常 RPC ベンチを汚さずに framing / header parse / protobuf merge / streaming frame split をローカル CPU 計測できるようにした。結果、unary framing と header parse はサブ μs で主因ではなく、streaming frame split は buffer を message ごとに `substr()` で詰める実装が 1000 messages で 1.239 ms まで伸びることを確認。`ServerStreamingCall` を buffer offset + pending offset 方式に変更し、同じ分解計測では 1000 messages split が 54.422 μs まで下がった。実 RPC では count=1000 が 1.40 ms → 1.357 ms と小幅改善で、残る支配要因は libcurl callback / `curl_multi_*` / Generator / protobuf object 生成側。詳細: `docs/benchmarks/hot-path-breakdown-2026-04-27.md`。
- **2026-04-27**: Phase 1 ベンチ基盤を再整理。`bench/run.sh` を通常入口として追加し、`lite` / `ext` / `compare` / `cold` / `warm` / `stream` / `stream-smoke` / `hot-path` の各スイートを Docker compose 内で実行して `var/bench-results/` にログ保存できるようにした。`bench/compare.sh` も互換入口として同じ実装へ委譲する。`tools/parse-phpbench-aggregate.php` で PHPBench aggregate ログを JSON/TSV に抽出し、`mode_ns`、`mem_peak_bytes`、`rstdev_percent` を機械比較できる形にした。`tools/compare-benchmark-baseline.php` と `bench/baselines/regression.json` を追加し、`cold` / `warm` / `stream-smoke` で php-grpc-lite 自身の回帰判定を実行できるようにした。Phase 1 はローカル再現基盤まで進行、CI のしきい値判定は残タスク。
- **2026-04-27**: PHPBench artifact 生成を `bench/phpbench-with-artifacts.sh` に分離。PHPBench 実行、aggregate parse、JSON/TSV 保存、任意の regression baseline 比較を同一コンテナ内で完結させ、ホストに `tee` したログを直後に別コンテナから bind mount 経由で parse する構成を廃止した。これにより bind mount 反映待ちの retry と `bash -c` に埋め込んだ ext-grpc 実行処理を不要にした。
- **2026-04-27**: `docs/code-reading-guide.md` を現行実装に合わせて更新。対象 commit を Phase 1 時点へ進め、server streaming の buffer offset / pending offset 実装、Channel-scoped curl handle reuse、mTLS/Spanner 検証、ベンチ基盤への参照を反映した。あわせて gRPC ライブラリとしての自己レビューを追加し、現状は非圧縮 unary + server streaming の最小実装として妥当だが、trailers-only error、`grpc-message` percent decode、HTTP status/content-type validation、client-side deadline、圧縮対応、binary metadata 互換は未達として明示した。
- **2026-04-28**: Phase 1 のベンチ追加を完了扱いに整理。既存の `cold` / `warm` / `stream` / `stream-smoke` / `hot-path` に加え、実用性能軸として `stream-slow`、`metadata`、`tls` を追加し、php-grpc-lite vs 公式 ext-grpc の実測結果を `docs/benchmarks/` に記録した。手元運用・継続比較基盤は完成とし、残タスクは CI で回す smoke の選定、regression baseline 運用、`mem_peak` の回帰判定に限定する。
- **2026-04-28**: regression baseline 運用入口として `bench/baseline.sh` を追加。`check` は `cold` / `warm` / `stream-smoke` の php-grpc-lite 側だけを実行して `bench/baselines/regression.json` と比較し、`mode_ns` と `mem_peak_bytes` の回帰を判定する。`update` は意図した性能変化や環境変更を受け入れる時だけ現在値から baseline を更新する。Phase 1 の残タスクは CI で `check` をどのタイミングで回すかの決定に絞る。
- **2026-04-28**: Phase 1 を完了扱いに変更。CI 実行設定は通常の保守運用タスクとして残すが、Phase 1 の完了条件はローカルで再現可能な継続比較、公式 ext-grpc 比較、php-grpc-lite 自身の regression baseline 運用が揃った時点で満たしたと判断する。Phase 2 は C 拡張化へ直接入らず、`docs/benchmarks/measurement-plan-phase2.md` の多軸計測で persistent pool / per-frame streaming / per-byte decode / header parser などの優先順位を決める。
- **2026-05-03**: Phase 2 native transport MVP比較を実施。current libcurl transport、nghttp2 native MVP PoC、公式 ext-grpcをlarge request unary / server streaming代表形状で比較し、default transportはnative nghttp2経路へ進める判断に更新した。その後、実装とQAを単純化するためlibcurl runtime経路は削除し、transportはnative 1系統に絞る方針へ更新した。MVP scopeは upload no-copy + poll loop、response compact/ring buffer、direct payload assembly。詳細: `docs/research/native-transport-mvp-comparison-2026-05-03.md`、`docs/native-transport-decision.md`、`docs/native-transport-design.md`。
- **2026-05-03**: native transportのactual `UnaryCall::wait()` / `ServerStreamingCall::responses()` surfaceを測るvariantをPhase 2比較runnerへ追加し、`100×100KiB` server streaming例外形状のfocused repeat runnerを追加した。native wrapperからserver stats trailer相当も公開し、PoC APIだけでなく本体surface経由でclient/server timingを並べられるようにした。詳細: `docs/research/native-surface-repeat-2026-05-03.md`。
- **2026-05-03**: native control semanticsをMVP surfaceで進めた。bench専用の `grpc_native_bench_unary_batch()` にdeadlineを追加してnative制御系を検証し、PHP wrapperでは `STATUS_DEADLINE_EXCEEDED`、API-level `cancel()`、missing `grpc-status` の `STATUS_UNKNOWN` 合成、HTTP/2 stream resetの基本status変換、TLS/mTLS明示未対応時の `STATUS_UNAVAILABLE` を扱う。server streamingはまだbatch drain後yieldなので、transport-level `RST_STREAM` と真のbackpressureはproduction streaming resource化後に残す。詳細: `docs/research/native-control-semantics-2026-05-03.md`。
- **2026-05-04**: native server streamingをC stream resource化し、`ServerStreamingCall::responses()` が `grpc_native_stream_next()` をpullしてmessageごとにyieldする経路へ切り替えた。`cancel()` は `RST_STREAM(CANCEL)` を送る。small SELECT代表形状(`1x100B` / `1x1KiB` / `1x4KiB` / `1x10KiB`)ではnativeがcurl/ext-grpcよりp50/p99とも良好、Spanner DML unary shapeでもnativeが最良。一方、`100x100KiB` server streamingなどlarge response surfaceではPoC direct/compactやext-grpcに対して未解明の差が残る。詳細: `docs/research/native-stream-resource-2026-05-04.md`。
- **2026-05-04**: metadata shape policyを確定。同一 key 複数 values は公式 ext-grpc PHP APIだと最後 value のみ可視だが、php-grpc-lite はgRPC仕様準拠を優先して `array<string, list<string>>` で全 values を保持する。native request metadataの畳み込みとC extension固定header buffer制限は修正済み。詳細: `docs/research/metadata-compatibility-gap-2026-05-04.md`。
