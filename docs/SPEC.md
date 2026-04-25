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

| Phase | 内容 | 完了条件 |
|---|---|---|
| 0 | 純 PHP PoC。libcurl + ext-curl で HTTP/2 を喋り、gRPC framing を PHP で実装。`Grpc\` 互換 API を提供 | unary と server streaming の sample が動く |
| 1 | ベンチマーク基盤整備 | レイテンシ/スループット/メモリ計測を CI で回せる |
| 2 | ホットパスの拡張化(framing, metadata, status 解釈) | ベンチで純 PHP より明確に速い |
| 3 | nghttp2 直接呼び出しに置き換え(必要と判断された場合のみ) | libcurl 依存除去 |

各フェーズの遷移はベンチマーク結果で判断する。Phase 3 は未確定(libcurl のままで十分かもしれない)。

### 4.2 HTTP/2 トランスポート

| フェーズ | トランスポート |
|---|---|
| 0–1 | libcurl(ext-curl 経由)+ HTTP/2 |
| 2   | libcurl(C から呼ぶ) |
| 3   | nghttp2 + 自前ソケット(条件付き) |

#### 重要な前提

- gRPC は status を **HTTP/2 trailers**(`grpc-status`, `grpc-message`)で返す。libcurl では `CURLOPT_HEADERFUNCTION` で trailer も含めて受け取れる(要 PoC で検証)。
- gRPC framing は `1 byte compressed-flag + 4 byte big-endian length + payload`。
- ALPN で `h2` を広告する必要がある。OpenSSL 1.0.2+ で対応(Trixie の OpenSSL 3.5 で要件十分)。

### 4.3 TLS 戦略

- システム OpenSSL を使用する。PHP の `ext-openssl` と同じ libssl を共有する。
- BoringSSL は同梱しない。
- 拡張フェーズ(Phase 2 以降)で nghttp2 を使う場合も nghttp2 は TLS を含まないため、OpenSSL を直接使えばよい。
- mTLS(client cert)、CallCredentials によるトークン付与は OpenSSL の機能で完全に実現可能。

### 4.4 Protobuf

- `protobuf` PECL 拡張(`pecl install protobuf`)を前提とする。
- メッセージ型は `Google\Protobuf\Internal\Message` 互換のものを扱う。
- スタブコード生成は既存の `protoc-gen-php-grpc` 互換出力を当面そのまま受け入れる(独自コードジェネレータは後続検討)。

### 4.5 API 互換性

`google/gax` および `google/cloud-*` が利用する `Grpc\` 名前空間 API を網羅する。

#### 互換必須クラス/定数(現時点の見立て)

| 種別 | シンボル | 互換必須度 | 備考 |
|---|---|---|---|
| Channel | `Grpc\Channel` | 必須 | hostname, opts |
| Channel | `Grpc\ChannelCredentials` | 必須 | `createSsl`, `createInsecure`, `createDefault` |
| Call | `Grpc\BaseStub` | 必須 | `_simpleRequest`, `_serverStreamRequest` 等 |
| Call | `Grpc\UnaryCall` | 必須 | `start`, `wait`, `cancel`, `getMetadata`, `getTrailingMetadata` |
| Call | `Grpc\ServerStreamingCall` | 必須 | `start`, `responses`, `getStatus` |
| Call | `Grpc\ClientStreamingCall` | 後回し | |
| Call | `Grpc\BidiStreamingCall` | 後回し | |
| Auth | `Grpc\CallCredentials` | 必須 | `createFromPlugin` が gax で使われる |
| 時刻 | `Grpc\Timeval` | 必須 | deadline 表現 |
| Status | `Grpc\STATUS_*` 定数 | 必須 | OK/CANCELLED/UNKNOWN/... の 17 種 |
| Interceptor | `Grpc\Interceptor`, `Grpc\Intercept` | 必須 | gax が触る |
| 内部 | `Grpc\CallInvoker` | 観測振る舞いのみ | 直接の API 互換は不要 |

> 各シンボルの正確な互換要件は `google/gax` のソースから抽出した一覧を別表(TBD: `docs/api-surface.md`)で管理する。

---

## 5. 環境

### 5.1 対応 PHP バージョン

- PHP **8.3+**(NTS 主軸)
- ZTS は将来検討(現時点では非対象)

### 5.2 開発環境

- Debian **Trixie** ベースのコンテナ
- ベースイメージ: `php:8.3-cli-trixie`
- ホスト OS 非依存(macOS の LibreSSL/Homebrew には依存しない)
- 起動方法: `docker compose run --rm dev`
- 同梱ツール: build-essential, autoconf, pkg-config, libcurl4-openssl-dev, libssl-dev, libnghttp2-dev, protobuf-compiler, composer, pecl protobuf 拡張

### 5.3 ビルド/配布

- 開発時: ローカルビルド(`phpize && ./configure && make`)
- 配布: 最終的に PIE(`pecl/pie`)経由

---

## 6. 未決事項

- [ ] `google/gax` から呼ばれる `Grpc\` API の正確な一覧化(grep ベースで `docs/api-surface.md` に出力)
- [ ] テスト用 gRPC サーバーの選定(Go の helloworld を `compose.yaml` に並べる案が有力)
- [ ] ベンチマーク手法とターゲット環境(計測対象、繰り返し回数、コンパレータ)
- [ ] Persistent channel pool の互換要件(ext-grpc は `grpc.use_local_subchannel_pool` 等の INI で制御)
- [ ] Interceptor の API 形状の精査(`Grpc\Intercept` は静的メソッド形)
- [ ] マルチアーキテクチャ対応(arm64 / amd64)
- [ ] エラーログ/デバッグ出力の方式

---

## 変更履歴

- **2026-04-25**: 初版作成。目的・スコープ・段階戦略・TLS/HTTP/2 方針・API 互換目標・開発環境を確定。
- **2026-04-25**: Dockerfile および compose.yaml を追加。開発環境セクションに起動方法と同梱ツールを追記。
