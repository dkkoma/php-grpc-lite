---
Status: Closed
Owner: Codex
Created: 2026-05-16
Branch: main
---

# CallCredentials plugin固定費削減

## 目的

GAX / google-cloud-spanner 経路で `call_credentials_callback` が使われる場合に、RPCごとに発生するCallCredentials bridge固定費を削減する。

## 背景

以前の `docs/issues/closed/2026-05-16-call-credentials-hotpath-followup.md` では、real Spanner経路ではC拡張側の `CallCredentials` callbackを主に使っていないと判断していた。しかし再確認すると、GAXは `credentialsWrapper` がある場合に `call_credentials_callback` を設定し、公式 `grpc/grpc` wrapperの `AbstractCall` が `Grpc\CallCredentials::createFromPlugin()` / `Call::setCredentials()` を呼ぶ。

そのため、`grpc_lite_merge_call_credentials_metadata()` の固定費は実ワークロードでも発生しうる。

## スコープ

- `grpc_lite_merge_call_credentials_metadata()` の明らかな固定費を削減する。
- gRPC CallCredentials plugin互換性を維持する。
- service URLは `scheme://authority/package.Service` というservice単位の値として扱う。
- unary / server streaming / franken-go bridgeの既存挙動を壊さない。

## 非スコープ

- plugin callback自体の実行回数を省略しない。authorization metadataはRPCごとに変わりうるため、callback呼び出しは維持する。
- GAX / grpc/grpc PHP wrapper側のAPIは変更しない。
- credential token cacheやgoogle-auth側の挙動は変更しない。

## 計画

- Channel内にCallCredentials service URL cacheを持つ。
- `grpc_lite_merge_call_credentials_metadata()` で毎回 `strpprintf()` していたservice URL生成をcache lookupに置き換える。
- callback zvalの不要なcopy/dtorを削減する。
- PHPTでCallCredentials callbackのservice URL / method name互換性を確認する。
- C static analysis / PHPT / 主要ベンチで検証する。
- レビュー指摘を `docs/reviews/issues/` に残し、必要な修正を反映する。

## 進捗

- [x] 実装
- [x] テスト
- [x] ベンチ
- [x] レビュー
- [x] クローズ

## 判断ログ

- service URL cacheはCall単位ではなくChannel単位に置く。CallはRPCごとに作られるため、Call単位cacheではRPC間の固定費削減にならない。
- callback呼び出しは削らない。CallCredentials pluginはRPCごとのmetadata生成契約であり、token expiryやaudience依存を壊せない。

## 完了条件

- CallCredentials callbackの互換性テストが通る。
- C static analysis / PHPTが通る。
- 主要ベンチで悪化がない、またはCallCredentialsあり経路で改善が説明できる。
- レビューでBlocker / Highが残っていない。

## 実装結果

- `grpc_lite_get_call_credentials_service_url()` を追加し、Channel単位でservice pathごとのservice URLをcacheするようにした。
- service URLは `grpc.default_authority` があればそれを優先し、なければtarget authorityを使う。method nameは従来どおりfull method pathを渡す。
- callbackの実行はRPCごとに維持した。authorization metadataの生成契約は変えていない。
- callback zvalは `call_user_function()` 中にlocal zvalへ `ZVAL_COPY()` してpinする形に戻した。service URL cacheだけを固定費削減対象にした。
- `cpu-micro` にCallCredentialsありのTLS unary / server streamingケースを追加した。ext-grpcがinsecure channel上のCallCredentialsを拒否するため、CallCredentialsケースはsecure targetで測る。
- TLS fixture / secure client初期化はCallCredentials secure case実行時だけに遅延した。

## 検証

- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-static-analysis.sh`: PASS
- `BENCH_TAG=call-credentials-hotpath-final-20260516 BENCH_OTEL_RUN_ID=call-credentials-hotpath-final-20260516 BENCH_IMPLEMENTATION=php-grpc-lite ./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100`: PASS

### ベンチ結果抜粋

| measurement | calls | cpu_us/call | wall_us/call |
|---|---:|---:|---:|
| small_unary_100b | 2000 | 17.9 | 61.1 |
| small_unary_100b_call_credentials | 2000 | 18.5 | 48.3 |
| small_streaming_1x100b | 2000 | 11.8 | 35.4 |
| small_streaming_1x100b_call_credentials | 2000 | 16.1 | 41.5 |

## 判断

- 採用する。CallCredentials callback呼び出し自体は残し、service URL生成の固定費だけをChannel-owned cacheへ移したため、per-RPC auth metadata semanticsを壊さない。
- 以前の `docs/issues/closed/2026-05-16-call-credentials-hotpath-followup.md` の「実経路では主に使っていない」という判断は古い。GAXは `credentialsWrapper` がある場合に `call_credentials_callback` を設定するため、この経路は実ワークロード対象として扱う。

## レビュー

- `docs/reviews/issues/2026-05-16-call-credentials-plugin-hotpath-domain-review-codex.md`: 初回 High 2 / Medium 1。修正後の再レビューで Blocker 0 / High 0 / Medium 0 / Low 0。

## 完了

- 完了日: 2026-05-16
- 修正コミット: this commit
