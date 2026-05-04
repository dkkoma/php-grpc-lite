# Native resource lifecycle cleanup (2026-05-04)

## 目的

native transportをproduction C extensionへ寄せる前に、PoC extension内のresource ownershipとfailure pathを見直す。

対象:

- persistent channel cache
- unary call中のchannel busy状態
- stream resource destructor
- `poc_client` が持つresponse body / queued payload / metadata / `grpc-message`
- error pathでの `nghttp2_session_set_user_data()` 解除

## 見直し結果

既存実装は通常成功経路では動いていたが、以下が弱かった。

- unary経路で `channel->busy` を明示管理していなかった。
- `perform_poc_channel_unary()` の一部failure pathで、stack上の `poc_client` が持つmetadata / `grpc_message` / response buffer cleanupが分散していた。
- stream resource destructorとunary cleanupで解放対象の列挙が重複していた。
- direct diagnostic `nghttp2_poc_unary()` にも同じmetadata cleanup漏れがあり得た。

## 実装

- `cleanup_poc_client()` を追加し、以下を一箇所で解放する。
  - `response_payload`
  - `response_raw_payload`
  - queued response payloads
  - queued raw payloads
  - collected metadata
  - `grpc_message`
  - `smart_str body`
- `destroy_poc_stream()` は `cleanup_poc_client()` を使う。
- `perform_poc_channel_unary()` はcall中に `channel->busy = true` とし、成功・失敗どちらでも `user_data` とbusy状態を解除する。
- `nghttp2_submit_request()` / `nghttp2_session_send()` / `nghttp2_session_mem_recv()` failure pathで `cleanup_poc_client()` を通す。
- batch diagnostic loopでもiterationごとに `cleanup_poc_client()` を通し、前回iterationのmetadata / `grpc_message` を残さない。

## 追加テスト

`NativeTransportControlTest::testNativeStreamResourceDestructReleasesChannelBusyState`

確認内容:

1. 同一persistent channel keyでstreamを開く。
2. active stream中の2本目openが `active stream` エラーになる。
3. 1本目resourceを `unset()` する。
4. 同一keyで次のstreamを開ける。
5. 次streamからpayloadを読める。

これにより、stream resource destructorが `RST_STREAM(CANCEL)` と `channel->busy=false` を実行し、persistent channelを次RPCへ戻せることを確認する。

## 検証

```bash
docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && make -j2'
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so vendor/bin/phpunit tests/Integration/NativeTransportControlTest.php
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so vendor/bin/phpunit
docker compose run --rm dev vendor/bin/phpunit
```

結果:

- `NativeTransportControlTest`: 25 tests / 78 assertions / 1 skipped
- extension loaded full PHPUnit: 59 tests / 198 assertions / 1 skipped
- normal full PHPUnit: 59 tests / 125 assertions / 23 skipped

## Lifecycle stress runner

production hardening用に、同じfixtureを通常実行とValgrind実行で使えるrunnerを追加した。

```bash
BENCH_TAG=20260504-native-lifecycle-stress ITERATIONS=100 MESSAGE_COUNT=20 PAYLOAD_BYTES=1024 ./bench/phase2/check-native-lifecycle-stress.sh
VALGRIND=1 BENCH_TAG=manual-valgrind ITERATIONS=5 ./bench/phase2/check-native-lifecycle-stress.sh
```

`VALGRIND=1` はdev image内に `valgrind` がある場合だけ実行する。現行dev imageには未同梱なので、必要時にdebug imageへ追加して使う。

通常stressで見るscenario:

- `full_drain_repeat`: streamを最後まで読み切る。
- `break_unset_repeat`: Generatorを途中breakして破棄し、次RPCが通ることを確認する。
- `cancel_mid_stream_repeat`: 1 message受信後に `cancel()` し、次RPCが通ることを確認する。
- `timeout_repeat`: stream中deadline exceeded後、次RPCが通ることを確認する。
- `raw_resource_unset_repeat`: C stream resourceを直接 `unset()` し、同一persistent channel keyで次streamを開けることを確認する。

2026-05-04 実測:

summary: `var/bench-results/phase2-native-lifecycle-stress-20260504-native-lifecycle-stress.json`

| scenario | iterations | failures | wall ms | PHP memory delta | RSS max delta | FD delta |
|---|---:|---:|---:|---:|---:|---:|
| `full_drain_repeat` | 100 | 0 | 14.7 | 260560 B | 672 KiB | 1 |
| `break_unset_repeat` | 100 | 0 | 13.1 | 112200 B | 48 KiB | 0 |
| `cancel_mid_stream_repeat` | 100 | 0 | 15.7 | 0 B | 0 KiB | 0 |
| `timeout_repeat` | 100 | 0 | 1552.0 | 0 B | 16 KiB | 0 |
| `raw_resource_unset_repeat` | 100 | 0 | 41.3 | 0 B | 32 KiB | 1 |

FD delta `1` はpersistent channelが1本残るケースで、iterationに比例した増加ではない。途中break、cancel、timeoutではFD deltaは0で、stream resource destructor / cancel pathがchannel busy状態を解除して次RPCへ戻せている。

## 判断

Phase 2 MVPとしてのnative resource lifecycle整理は完了と扱う。

残るrelease hardeningは、production packaging後にASAN/Valgrind相当のnative memory check、より長時間のstress、複数request/FPM lifecycleでの実機確認として扱う。
