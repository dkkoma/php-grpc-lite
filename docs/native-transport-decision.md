# Native Transport Decision

## Decision

`php-grpc-lite` の本実装transportは native nghttp2 transport をdefaultにする方針で進める。

ただしこれはPhase 2の設計判断であり、drop-in release defaultにはまだしない。release defaultにするには、native extension packaging、TLS/mTLS、true server streaming resource、Channel/session reuse、control semantics互換を満たす必要がある。

ただし、libcurl経路はfallbackではなく明示的な安定経路として残す。ユーザーはworkloadや運用安定性に応じて `native` / `curl` を選択できる。

理由:

- request/responseのlarge payload pathで、libcurl経由では避けにくいbuffer/copy/進行単位の制約がある。
- nghttp2 direct PoCでは、large request unaryと多くのserver streaming large response形状でext-grpc同等レンジまで到達した。
- small SELECT相当のserver streamingは実運用上の主経路なので、large payloadだけでなくsmall streamingでもext-grpc同等または優位をrelease判断の主軸にする。
- `compact/ring buffer + direct payload assembly` により、server streamingのmemory保持と二重copyを抑えられる。
- libcurl経路はcold固定費やsmall unaryには有効だが、Phase 2で見ているlarge payload transport構造の本筋改善にはならない。
- 一方で、libcurl経路は既に互換性検証済みの範囲が広く、native移行期の安全な選択肢として価値がある。

Transport option:

```php
new GreeterClient($target, [
    'credentials' => ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'native', // Phase 2 target default
]);

new GreeterClient($target, [
    'credentials' => ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'curl',   // explicit stable route
]);
```

自動fallbackはしない。`native` を選んでnative未対応機能やtransport errorに当たった場合、黙ってcurlへ落とさず明示的に失敗させる。

## Release Default Gates

nativeをdrop-in release defaultにする前に満たす条件:

- native extension packagingが通常利用できる。
- TLS / mTLS がlibcurl経路と同等に通る。
- server streamingがbatch drain後yieldではなく、messageごとにtransportからyieldできる。Phase 2 stream resourceでMVP検証済み。
- slow consumer時にmemory upper boundとbackpressureが検証済み。stream resourceでpull型backpressureは導入済み、memory upper boundの長時間検証は残る。
- `cancel()` がtransport-level `RST_STREAM(CANCEL)` として働く。Phase 2 stream resourceでMVP検証済み。
- Channel lifetimeでHTTP/2 session/socketを再利用できる。unary simple経路はC側persistent channelでrequestまたぎ再利用する。production server streaming resourceも同じlifecycleへ載せる。
- RST_STREAM / missing trailers / metadata / status / deadlineの互換性をext-grpcまたはlibcurl経路と照合済み。
- small SELECT代表形状、特に1 messageの `1x1KiB` / `1x4KiB` / `1x10KiB` server streamingで、ext-grpc同等または優位のp50/p99とthroughputを示せる。

## MVP Scope

Native transport MVPに入れるもの:

1. **Request upload no-copy + poll loop**
   - gRPC 5B headerとprotobuf payloadを巨大concatしない。
   - nghttp2 data providerでheader slice / payload sliceを供給する。
   - partial writeをtransport stateに保持し、nonblocking poll loopで再開する。

2. **Response compact/ring buffer**
   - many-small / long stream用。
   - consumed bytesを保持し続けない。
   - append-only response body bufferは採用しない。

3. **Response direct payload assembly**
   - large response payload用。
   - gRPC frame headerでpayload長を確定した時点で最終payload bufferへDATAを組み立てる。
   - `DATA -> body buffer -> payload string` の二重copyを避ける。

4. **Insecure h2c benchmark path**
   - まずGo test-serverのcontrolled benchmarkでlibcurl/current、MVP、ext-grpcを比較する。
   - TLS/mTLS等はMVP後のcompatibility integrationで入れる。

## Deferred

Native transportに残すがMVP必須ではないもの:

- **bounded read-ahead**
  - 一部large/long streamでp99改善がある。
  - defaultでは無効。後でshape-aware heuristicまたはoptionとして再検討する。

- **adaptive receive window / buffer tuning**
  - receive window拡大とbuffer sizeは効くが、固定値だけで全case最適にはならない。
  - MVPでは安全なdefaultを置き、decision run後に調整する。

- **shared event loop / multiplex scheduler**
  - concurrent workloadのthroughputには効く。
  - 単一call latency改善の本筋ではない。
  - channel transportの次段階として扱う。

## Explicit Stable Route

本実装のdefaultはnativeだが、libcurl経路は以下の目的で維持する。

- workloadごとの明示的な選択肢。
- native移行期の安定経路。
- 互換性比較のoracle。
- production rollback path。

これは自動fallbackではない。ユーザーまたはベンチが `php_grpc_lite.transport=curl` を指定した場合だけlibcurl経路を使う。

## Known Exceptions

- `100×100KiB` server streamingはactual surface repeat 1回ではnativeがcurl/ext-grpcに勝っていない。server last p99も同時に悪化しており、client CPUだけでは説明できない。decision evidenceに昇格するには `REPEATS>=3` で再取得する。
- 現MVP actual surfaceのserver streamingは `NativeTransport::unaryBatch()` でbatch drain後にyieldする。PoC batch APIは診断線であり、採用判断はactual surface resultを優先する。

## Rejected

現時点で採用しないもの:

- **PHP 8.5 persistent curl share**
  - libcurl経路は明示的な安定経路として残すが、target architectureはnativeであり、persistent curl shareを主改善策にはしない。

- **unbounded read-ahead**
  - many-small / long streamでqueue waitとdelivery latencyを悪化させる。

- **per-call transport thread**
  - PoCで安定した改善が出ていない。
  - thread起動/join、queue delivery、Zend API制約の複雑さに見合わない。

## Evidence

- Large request unary MVP comparison: `docs/research/native-transport-mvp-comparison-2026-05-03.md`
- Native actual surface / 100×100KiB repeat: `docs/research/native-surface-repeat-2026-05-03.md`
- Native control semantics: `docs/research/native-control-semantics-2026-05-03.md`
- Upload no-copy + poll loop: `docs/research/native-transport-unary-large-request-conclusion-2026-05-03.md`
- Native stream resource: `docs/research/native-stream-resource-2026-05-04.md`
- Server streaming goal comparison: `docs/research/nghttp2-poc-server-stream-goal-comparison-2026-05-03.md`
- Small SELECT streaming comparison: `docs/research/small-select-streaming-comparison-2026-05-03.md`
- Spanner emulator streaming shape: `docs/research/spanner-emulator-streaming-shape-2026-05-03.md`
- Spanner DML unary shape comparison: `docs/research/spanner-dml-unary-shape-comparison-2026-05-03.md`
- Bounded read-ahead: `docs/research/nghttp2-poc-server-stream-bounded-read-ahead-2026-05-03.md`
- Shared event loop / multiplex: `docs/research/curl-multiplex-shared-event-loop-2026-05-03.md`
