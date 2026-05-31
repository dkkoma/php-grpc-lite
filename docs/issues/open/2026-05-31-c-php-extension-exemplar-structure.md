# C/PHP拡張のお手本プロジェクト化

- Status: Open
- Created: 2026-05-31
- Branch: main
- Owner: Codex

## Background

`php-grpc-lite` は、公式 `grpc/grpc` PHP wrapper と repository root の source-built `grpc` extension による gRPC client implementationであり、unary、server streaming、TLS、mTLS、deadline、metadata、Spanner emulator経路まで実機検証済みである。

2026-05-29 のC保守性改善により、repository root extension化、`.c` include廃止、production / diagnostic translation unit分離、`src/` へのC source集約、`wrapper_adapter` 命名整理、`grpc_exchange_state` / `grpc_result` / `bench_call` 分割は完了している。

一方で、初学者から上級者までが「C言語およびPHP拡張のお手本」として納得する構造にするには、現在の実装はまだ次の点が弱い。

- `src/transport.c` と `src/transport.h` が大きく、connection、persistent cache、nghttp2 callbacks、socket/TLS、request header、response parser、metadata、status resultまで広く持っている。
- `src/grpc_exchange_state.h` の `grpc_call` が、gRPC call semantics、HTTP/2 stream state、parser state、write state、metadata、deadline、I/O error、bench-only fieldをまとめて持っている。
- active stream、nghttp2 stream user data、persistent cache detachment、server streaming resource destructorのownership invariantがコードを追えば分かるが、型や小さいAPIとして読める状態ではない。
- protocol parse中に `RST_STREAM` 送信などのHTTP/2 side effectが混ざり、protocol classificationとtransport actionの境界が教材として見えづらい。
- test fixtureの意味、`x-bench-*` metadata control protocol、raw fixture ports `50055`-`50060` がまとまったカタログになっていない。
- coverage量は強いが、仕様項目とPHPT / PHPUnit / C unit / fuzz / sanitizer / ZTS gateの対応表がない。

## Goals

- C/PHP拡張プロジェクトとして、ファイル境界、ヘッダ境界、所有権、lifecycle、検証層を読めば学べる構造にする。
- 初学者には「どこから読むか」「PHP extension object lifecycleはどこを見るか」「PHPTとC unitの違いは何か」が分かる状態にする。
- 中級者には「Cのtranslation unit、internal header、static関数、opaque化、resource destructor、Zend object ownership」の実例として分かる状態にする。
- 上級者には「HTTP/2/gRPC domain model、deadline、metadata/status、RST_STREAM/GOAWAY/EOF lifecycle、ZTS、diagnostic boundary」の責務分離が納得できる状態にする。
- 性能影響があり得る構造変更は、before/after計測と採否基準を持つ別タスクとして扱う。

## Child Issues

このissueは親issueとして扱い、実作業は以下の子issueに分ける。

| Child issue | Scope | Performance risk |
|---|---|---|
| `docs/issues/closed/2026-05-31-exemplar-docs-fixtures-and-reading-guide.md` | fixture catalog、verification matrix、reading guide、fuzz README | なし |
| `docs/issues/closed/2026-05-31-exemplar-test-discoverability-and-gates.md` | 長いPHPT、PHPUnit CI、lifecycle/slow-consumer gate整理 | CI時間・flake riskあり |
| `docs/issues/closed/2026-05-31-exemplar-transport-header-boundaries.md` | `transport.h` / `common.h` のheader boundary整理 | 宣言移動のみならなし。hot path変更時は別途計測 |
| `docs/issues/closed/2026-05-31-exemplar-connection-ownership-model.md` | connection / stream / resource ownership invariant整理 | bookkeeping変更時は計測候補 |
| `docs/issues/closed/2026-05-31-exemplar-grpc-call-exchange-state-map.md` | `grpc_call` field mapと分割採否 | 高。実装時before/after必須 |
| `docs/issues/open/2026-05-31-exemplar-grpc-call-field-layout-hotpath.md` | `grpc_call` field order / hot-cold layoutの測定と採否 | 高。実装時before/after必須 |
| `docs/issues/open/2026-05-31-exemplar-protocol-classification-boundary.md` | protocol classification と transport action分離 | 高。実装時before/after必須 |

## Non-Goals

- ext-grpc の数値へ近づけること自体を目的にしない。
- runtime transportを追加しない。nghttp2 + socket/TLS の1系統を維持する。
- libcurl fallback、transport selection option、環境変数によるtransport切替を追加しない。
- client streaming / bidi streamingをこのissueでは実装しない。
- 構造整理のついでにgRPC API互換性、metadata shape、deadline semantics、TLS/mTLS semanticsを変えない。
- performance改善を名目にした未計測のmicro optimizationを行わない。

## Plan

### 1. Documentation-first improvements

性能影響なし。先に着手してよい。

- `docs/test-fixtures.md` を追加する。
  - `50051`: normal h2c gRPC
  - `50052`: TLS
  - `50053`: mTLS
  - `50054`: non-gRPC h2c / HTTP validation
  - `50055`-`50060`: raw lifecycle fixtures
  - `x-bench-*` metadata trigger、expected status、owning PHPT/PHPUnitを表にする。
- `docs/verification-matrix.md` を追加する。
  - unary / server streaming × deadline / metadata / status / compression / resource limit / RST_STREAM / GOAWAY / EOF / TLS / mTLS / slow consumer / ZTS。
  - PHPT / PHPUnit / C unit / fuzz / sanitizer / release gateの対応をリンクする。
  - 未カバーまたは薄いセルを明示する。
- `docs/code-reading-guide.md` に beginner / intermediate / advanced の読み順を追加する。
- `tests/fuzz/README.md` を追加し、selector byte、seed corpus、CI smokeの目的を説明する。

### 2. Test discoverability improvements

主にテスト構造の整理。原則として性能影響なしだが、fixture server挙動を変える場合はPHPT/PHPUnit再実行を必須にする。

- 長いPHPTを目的別に分けるか、scenario tableを導入する。
  - `020-request-metadata-control.phpt`: positive round-trip、invalid metadata、mutation/isolationを分ける候補。
  - `024-control-semantics.phpt`: connection refusal、GOAWAY/EOF、RST、abandoned streamを分ける候補。
- PHPUnit integration suiteのCI上の扱いを整理する。
  - fast non-Spanner groupとSpanner emulator groupの分離を検討する。
  - Native QAに入れるか、scheduled/manual gateにするかを決める。
- lifecycle / slow-consumer scriptsを「gate」か「measurement-only」か明確にする。
  - gateにする場合はFD/RSS/latencyなどのthresholdを実装する。
  - measurement-onlyなら名称・docsを合わせる。

### 3. Header and source boundary cleanup

挙動変更なしを原則にするが、hot pathに関数境界やinline/cache変更を入れる場合は性能計測対象にする。

- `src/transport.h` を責務別に分割する。
  - 候補: `h2_connection.h`, `persistent_connection_cache.h`, `h2_request_headers.h`, `h2_callbacks.h`, `grpc_response_parser.h`, `grpc_metadata.h`, `transport_io.h`。
  - 最初は宣言移動とinclude整理だけにし、実装ロジックを変えない。
- `src/common.h` の巨大includeを段階的に薄くする。
  - PHP/Zend、OpenSSL、nghttp2、socket/system includeを必要なヘッダへ寄せる。
- `src/status_core.h` の依存を明示する。
  - `grpc_call` に依存するなら「status taxonomy over exchange state」として名前やdocを合わせる。
  - pure status mappingにできるなら入力DTO化を検討する。
- `src/transport.c` 冒頭の重複prototypeと古いコメントを整理する。
- internal API名のprefixを揃える。
  - 例: `server_streaming_call_open_resource()` などを `grpc_lite_` prefix方針に合わせるか、例外ルールを明記する。

### 4. Ownership model extraction

性能影響の可能性あり。実装前に具体案、影響範囲、検証計画を追記する。

- active stream registration / unregister / owner clear / detached connection destroy のinvariantを小さいAPIへ寄せる。
- unaryとserver streamingで `call->connection` を残す/消す差を明文化し、必要ならowner typeまたはstate transition helperで表現する。
- persistent cache entry削除後のconnection lifetimeを、読みやすい所有権モデルにする。

### 5. Exchange state decomposition

性能影響の可能性が高い。単なるfield groupingでもcache localityやhot path accessに影響し得るため、before/after計測を必須にする。

- `grpc_call` を概念別のsub-structへ分ける案を検討する。
  - stream lifecycle
  - status / validation flags
  - response metadata
  - response parser
  - request writer
  - deadline / I/O error
  - bench-only observation
- まずはdoc上のfield mapを作り、直接分割は別issueまたはこのissue内の明示phaseで扱う。
- 分割する場合はunary/server streaming代表bench、PHPT、C coverage、domain model reviewを必須にする。

### 6. Protocol classification vs transport action split

性能影響と挙動差分の可能性あり。HTTP/2/gRPC domain model review必須。

- response parserは「何が起きたか」を分類する。
- transport layerは分類結果に基づいて `RST_STREAM` / connection dead / draining / cache detachを判断する。
- metadata too large、message too large、invalid content-type、unsupported compression、malformed frameなどを対象に、現在のside effect位置を棚卸しする。

## Performance-sensitive Tasks

以下は、実装前に仮説、対象ワークロード、before計測、採否基準をissueへ追記する。

| Task | Why performance-sensitive | Required before/after |
|---|---|---|
| `grpc_call` field分割 / sub-struct化 | hot pathで頻繁に触るfieldのcache localityと分岐に影響する | `spanner-shape`, `metadata-header`, unary/server streaming microbench |
| response parser分離 | DATA chunk処理、message framing、metadata validationの呼び出し境界が増える可能性 | large streaming, server streaming small messages, unary 100B |
| request header builder分離 | request hot path allocation/copyに影響する可能性 | `metadata-header`, Spanner shape unary/streaming |
| ownership helper化 | active stream bookkeepingやcleanup pathに分岐が増える可能性 | PHPT lifecycle + representative unary/server streaming |
| lifecycle/stress threshold追加 | gate時間や環境依存flakeに影響する | CI smoke time, local release hardening time |
| PHPUnit CI追加 | CI時間とservice readinessに影響する | CI duration, flaky rate, Spanner emulator reset手順 |

### Benchmark Selection Policy

影響が少しでもありそうな作業は、次の順で判断する。

1. 既存benchmarkで評価できるなら、それをbefore/afterに使う。
2. 既存benchmarkでは対象hot pathが埋もれる場合は、実装前にsmall caseを追加する。
3. benchmark追加自体もissue単位の作業として扱い、何を観測するためのcaseかを記録する。
4. production buildとdiagnostic / bench buildでlayoutやcode pathが違う場合、diagnostic結果は原因分解の補助に留める。
5. 改善が測定誤差内なら採用しない。構造の説明性だけで採用する場合も、少なくとも代表benchでregressionがないことを条件にする。

残タスクごとの初期判断:

| Remaining task | Use existing benchmark first | Add benchmark/case when |
|---|---|---|
| transport header declaration move | runtime benchmark不要。build/static analysis/C unit/PHPTを優先 | request/header/parser/I/Oの関数境界、inline、cache policyに触るなら別issueで追加 |
| connection ownership helper | `cpu-micro`, `spanner-shape`, `throughput-unary`, `throughput-streaming` | stream registration/close固定費が埋もれるなら `tiny_unary_0b`, `tiny_streaming_1x0b` を追加 |
| `grpc_call` field layout | `cpu-micro`, `metadata-header`, `payload-*`, `spanner-shape` | 0B, 10-message streaming, metadata CPU/call, error/status detail caseを追加 |
| protocol classification boundary | `metadata-header`, `payload-*`, `large-streaming`, `spanner-shape`, `cpu-micro` | invalid protocol/error pathを固定回数で見る `protocol-error-micro` 相当を追加 |

## Progress

- 2026-05-31: プロジェクト全体、C source/header、PHPT/C unit/fuzz/PHPUnit/tools/testをread-only調査。
- 2026-05-31: C/PHP拡張のお手本化に必要な改善を本issueへ整理。
- 2026-05-31: 作業単位ごとに6本の子issueへ分割。
- 2026-05-31: fixture / verification / reading guide / fuzz README整備の子issueを完了。
- 2026-05-31: test discoverability / gate整理の子issueを完了。
- 2026-05-31: `grpc_call` exchange state mapの子issueを完了。field分割は未実施。
- 2026-05-31: transport header boundaryの方針docを追加。宣言移動は未実施のため子issueはopen継続。
- 2026-05-31: `grpc_call` field layout hot path最適化をperformance-sensitiveな後続issueとして追加。
- 2026-05-31: 残タスクごとに、既存benchmarkで判断する条件とsmall case追加が必要な条件を明文化。
- 2026-05-31: transport header boundary issueを完了。`h2_request_headers` 分離は採用し、persistent connection cache分離はreject。
- 2026-05-31: connection / stream ownership model issueを完了。`grpc_call` field mapへstate machineを統合し、helper化は採用しない判断を記録。

## Verification

現時点では調査とissue作成のみ。コード変更・テスト実行なし。

今後の標準検証候補:

- `./tools/test/check-native-development-gate.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-coverage.sh`
- `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`
- HTTP/2/gRPC transportに触る変更では `docs/protocol-model-review-guide.md` に沿ったdomain model review
- 性能影響がある変更では同条件のbefore/after benchmark

## Decision Log

- 2026-05-31: お手本化の第一歩は性能改善ではなく、構造・所有権・fixture・verification matrixの可視化とする。
- 2026-05-31: 性能影響があり得る構造変更は、documentation/test discoverability作業から切り分け、before/after計測なしに採用しない。
- 2026-05-31: `transport.h` / `grpc_call` の広さは現時点の実装上のBlockerではないが、お手本プロジェクトとしては主要な改善対象とする。
- 2026-05-31: 親issueは全体方針と進捗管理に限定し、実装・検証・close判定は子issue単位で行う。

## Close Criteria

- `docs/test-fixtures.md` が追加され、fixture port、metadata trigger、expected behavior、owning testsが分かる。
- `docs/verification-matrix.md` が追加され、主要HTTP/2/gRPC semanticsとテスト/gateの対応が分かる。
- code reading guideに習熟度別の読み順が追加される。
- fuzz corpus semanticsがREADME化される。
- `transport.h` の分割方針が実装または明示的な後続issueへ切り出される。
- `grpc_call` field mapと分割採否が記録される。実装する場合はbefore/after計測が記録される。
- lifecycle / slow-consumer / PHPUnit CIの扱いがgateまたはmeasurement-onlyとして明確になる。
- HTTP/2/gRPC domain model reviewでBlocker / High / Medium / Lowがnoneになる。
- 必要な検証結果を追記し、`Status: Closed` にして `docs/issues/closed/` へ移動する。
