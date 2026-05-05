# Native Transport Decision

## Decision

`php-grpc-lite` の本実装transportは native nghttp2 transport の1系統にする。

ただしこれはPhase 2の設計判断であり、drop-in releaseにはまだしない。releaseには、native extension packaging、長時間stress、large response tuning、production C extensionとしてのmemory checkを満たす必要がある。

libcurl runtime経路、transport option、環境変数によるtransport切替は持たない。extension未ロードやnative transport errorは別経路へfallbackせず、そのRPCの失敗として扱う。

理由:

- request/responseのlarge payload pathで、libcurl経由では避けにくいbuffer/copy/進行単位の制約がある。
- nghttp2 direct PoCでは、large request unaryと多くのserver streaming large response形状でext-grpc同等レンジまで到達した。
- small SELECT相当のserver streamingは実運用上の主経路なので、large payloadだけでなくsmall streamingでもext-grpc同等または優位をrelease判断の主軸にする。
- `compact/ring buffer + direct payload assembly` により、server streamingのmemory保持と二重copyを抑えられる。
- libcurl経路は比較・調査には有効だったが、本実装に残すと互換性QAと性能改善の分岐が増える。
- native extensionを前提にするなら、transportを1系統にした方がdrop-in surfaceの互換性とC extension hardeningへ集中できる。

## Release Default Gates

nativeをdrop-in release defaultにする前に満たす条件:

- native extension packagingが通常利用できる。
- TLS / mTLS がunary / server streamingとも通る。Phase 2 native compatibility gateで検証済み。
- server streamingがbatch drain後yieldではなく、messageごとにtransportからyieldできる。Phase 2 stream resourceでMVP検証済み。
- slow consumer時にconsumer-speed limitedになり、代表条件でmemoryが増え続けない。Phase 2 stream resourceでpull型backpressureを検証済み。長時間stressはrelease hardeningで扱う。
- `cancel()` がtransport-level `RST_STREAM(CANCEL)` として働く。Phase 2 stream resourceでMVP検証済み。
- Channel lifetimeでHTTP/2 session/socketを再利用できる。unary simple経路はC側persistent channelでrequestまたぎ再利用する。production server streaming resourceも同じlifecycleへ載せる。
- RST_STREAM / missing trailers / metadata / status / deadlineの主要互換性をext-grpc比較とnative compatibility gateで確認済み。metadata/status/compression/error semanticsの代表条件は検証済み。
- native resource lifecycleが整理され、stream resource destructor、unary failure path、persistent channel busy状態の代表条件と100 iteration stressを検証済み。production packaging後のnative memory checkerはrelease hardeningで扱う。
- small SELECT代表形状、特に1 messageの `1x1KiB` / `1x4KiB` / `1x10KiB` server streamingで、ext-grpc同等または優位のp50/p99とthroughputを示せる。

PHP userland codeはComposerで導入し、native extensionはこのrepositoryの `ext/grpc/` をclone後にsource buildする。Composer install時にnative extensionを自動buildしない。extension未導入環境で黙って別transportへfallbackして動かすのではなく、install/load段階で `extension_loaded('grpc')` を確認する。

release QA の判定表は `docs/release-qa-checklist.md` に集約する。

install手順は `docs/install-native-extension.md` に集約する。

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
   - まずGo test-serverのcontrolled benchmarkでnative MVP、ext-grpcを比較する。
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
  - 2026-05-05のnative mux spikeでは同一HTTP/2 session上の複数active streamは実装可能と確認したが、main比でwarm unary `+13.3%`、server streaming count=1000 `+6.3%` の退行が出た。
  - 現行public APIは同期blockingで、通常のFPM / FrankenPHP worker利用では同一実行コンテキスト内の複数in-flight RPCが自然には発生しないため、mainには採用しない。
  - async / concurrent RPC API、transport専用thread、または単一active stream fast pathを維持できる段階で再検討する。

## Transport Selection Guide

transportはnativeのみ。ただし、server streamingのlarge response bulk transferは、release時点でknown limitationとして扱い、実ワークロードに近いshapeでSLO内に入るかを事前確認する。

ここでのlarge判定はgRPC仕様上の制限ではなく、2026-05-04時点のcontrolled benchmarkに基づく運用閾値。

| 区分 | 目安 | 推奨 |
|---|---|---|
| small unary / small streaming | unary request/responseが数百B〜数KiB、server streamingが `1x100B` / `1x1KiB` / `1x4KiB` / `1x10KiB` 相当 | `native` を推奨 |
| many-small streaming | 1 messageが小さい。例: `1000x100B` / `10000x100B` | nativeで計測し、p99 SLOを確認 |
| single-large response | 1 messageが `>= 1MiB`、ただしmessage数は少ない | nativeで計測し、p99/throughputを確認 |
| medium-large streaming | 1 messageが `>= 64KiB`、またはstream totalが `>= 1MiB` | 事前ベンチ推奨 |
| large bulk streaming | 1 messageが `>= 64KiB` かつ stream totalが `>= 8MiB`、または `>= 50` messagesのlarge payload stream | known limitation対象。p99やworker占有がSLOに入るかを事前確認 |

代表例として `10x100KiB` はnativeがcurlより改善するがext-grpcより遅く、`100x100KiB` はnative stream resource surfaceがcurl/ext-grpcより明確に遅い。したがってlargeの実務閾値は「100KiB級messageが数十件以上、合計8〜10MiB級」と置く。

## Known Exceptions

- `100×100KiB` server streamingはnative stream resource化後もext-grpcよりstream p99が遅いケースがある。small SELECT主ワークロードのrelease判断とは分け、transport selection guideのlarge bulk streaming known limitationとして扱う。

## Rejected

現時点で採用しないもの:

- **PHP 8.5 persistent curl share / libcurl runtime route**
  - target architectureはnativeであり、curl routeを本実装に残さない。

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
- Native slow consumer / memory surface: `docs/research/native-slow-consumer-2026-05-04.md`
- Native compatibility gates: `docs/research/native-compatibility-gates-2026-05-04.md`
- Native resource lifecycle cleanup: `docs/research/native-resource-lifecycle-2026-05-04.md`
- Server streaming goal comparison: `docs/research/nghttp2-poc-server-stream-goal-comparison-2026-05-03.md`
- Small SELECT streaming comparison: `docs/research/small-select-streaming-comparison-2026-05-03.md`
- Spanner emulator streaming shape: `docs/research/spanner-emulator-streaming-shape-2026-05-03.md`
- Spanner DML unary shape comparison: `docs/research/spanner-dml-unary-shape-comparison-2026-05-03.md`
- Bounded read-ahead: `docs/research/nghttp2-poc-server-stream-bounded-read-ahead-2026-05-03.md`
- Shared event loop / multiplex: `docs/research/curl-multiplex-shared-event-loop-2026-05-03.md`
- Native mux / event loop spike: `docs/research/native-mux-event-loop-spike-2026-05-05.md`
