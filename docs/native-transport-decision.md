# Native Transport Decision

## Decision

`php-grpc-lite` の本実装transportは、libcurl継続ではなく native nghttp2 transport へ移行する。

理由:

- request/responseのlarge payload pathで、libcurl経由では避けにくいbuffer/copy/進行単位の制約がある。
- nghttp2 direct PoCでは、large request unaryとserver streaming large responseでext-grpc同等レンジまで到達した。
- `compact/ring buffer + direct payload assembly` により、server streamingのmemory保持と二重copyを抑えられる。
- libcurl継続案はcold固定費やsmall unaryには有効だが、Phase 2で見ているlarge payload transport構造の本筋改善にはならない。

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

- **persistent native channel pool**
  - PHP-FPM worker lifetimeでrequestをまたぐreuseに効く可能性がある。
  - ext-grpcのpersistent channel/subchannel poolに近い性質をnative側でどう持つかは別途設計する。

## Rejected

現時点で採用しないもの:

- **libcurl transport continuation**
  - 本実装の主経路には残さない。
  - 既存libcurl pathは比較対象・fallback期間のために残すことはあっても、target architectureではない。

- **PHP 8.5 persistent curl share**
  - libcurlを本実装に残さないため採用しない。

- **unbounded read-ahead**
  - many-small / long streamでqueue waitとdelivery latencyを悪化させる。

- **per-call transport thread**
  - PoCで安定した改善が出ていない。
  - thread起動/join、queue delivery、Zend API制約の複雑さに見合わない。

## Evidence

- Large request unary MVP comparison: `docs/research/native-transport-mvp-comparison-2026-05-03.md`
- Upload no-copy + poll loop: `docs/research/native-transport-unary-large-request-conclusion-2026-05-03.md`
- Server streaming goal comparison: `docs/research/nghttp2-poc-server-stream-goal-comparison-2026-05-03.md`
- Bounded read-ahead: `docs/research/nghttp2-poc-server-stream-bounded-read-ahead-2026-05-03.md`
- Shared event loop / multiplex: `docs/research/curl-multiplex-shared-event-loop-2026-05-03.md`
