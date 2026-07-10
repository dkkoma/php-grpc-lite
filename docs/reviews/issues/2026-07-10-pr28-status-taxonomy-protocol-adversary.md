# PR #28 status taxonomy protocol adversarial review 2026-07-10

## Scope

- `src/status_core.c`
- `src/transport.c`
- `src/grpc_exchange_state.h`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/unit/test_status_core.c`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/Integration/CompressionTest.php`
- `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md`
- `docs/design/grpc-call-exchange-state.md`
- `docs/design/protocol-classification-boundary.md`
- `docs/verification/compatibility-control-checklist.md`

## Reviewer Role

- HTTP/2 / gRPC status taxonomy protocol adversary

## Review Prompt Summary

- PR #28 (`origin/main...ce5872d`) を、公式 protocol / implementation に対する status taxonomy、`grpc-status` / HTTP status / RST_STREAM / clean END_STREAM / local transport error の優先順位、connection / stream / call lifecycle、missing-trailers 誤分類の観点から敵対的に確認した。既存レビューで解消済みの docs 更新と通常の no-trailers details 統合テストは重複指摘しない。修正commit `f5a2f751621cecbb447db7d89222df435fcf7849` で再レビューし、前回のMediumとDesign Decisionの解消、および新しいcall-local lifecycle fieldの設計資料反映を確認した。

## Issues

### REVIEW-20260710-001: `grpc-encoding` header の観測を「展開不能な圧縮 message」と誤分類している

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC status taxonomy protocol adversary`
- Finding: PR が `compressed_response_seen || unsupported_response_encoding` の共通 branch を `UNIMPLEMENTED` から `INTERNAL` へ変更し、C unit / PHPT / docsでも両者を同じ「client-side inability to process a server message」として固定した。しかし既存の `unsupported_response_encoding` は non-identity の `grpc-encoding` header を見ただけで成立する flag であり、展開不能な message を観測したことを意味しない。gRPC compression は per-message なので、compressed flag `0` の message は `grpc-encoding` にかかわらず非圧縮である。PR が引用する official 根拠は compressed message の失敗を `INTERNAL` とするもので、header observation まで `INTERNAL` 化する根拠にはならない。
- Evidence: PR の直接変更は `src/status_core.c:33-35` と `tests/unit/test_status_core.c:56-57` で、特に `unsupported_response_encoding` 単独かつ wire `grpc_status = OK` を新たに `INTERNAL` としてassertしている。その flag の既存成立点は `src/transport.c:2044-2052`。PR が期待値を変更した `tests/phpt/022-error-and-http-validation.phpt:53-56,127-130` の unsupported-encoding fixture は `poc/test-server/main.go:489-495` から compressed flag `0` の frameを送り、かつ `grpc-status` を送らないため、「未対応 algorithm で圧縮された message」を検証していない。公式 [PROTOCOL-HTTP2.md](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#requests) は compressed flag `0` を「Message bytes に encoding が行われていない」と定義し、公式 [compression.md](https://github.com/grpc/grpc/blob/master/doc/compression.md) も error 条件を unsupported algorithm で圧縮された *message* としている。grpc-go の [`checkRecvPayload()`](https://github.com/grpc/grpc-go/blob/master/rpc_util.go) は `compressionNone` では compressor lookup を行わず、`compressionMade` の場合だけ client-side `INTERNAL` を返す。さらに official ext-grpc 1.80.0 との実機比較では、`:status 200 + content-type application/grpc + grpc-encoding: gzip + Compressed-Flag=0 + grpc-status:0` は ext-grpc が response 非 null / `OK(0)` / details 空を返す一方、ce5872d は response null / `INTERNAL(13)` / `unsupported grpc-encoding: gzip` を返した。
- Expected model: `grpc_encoding` は response metadata observation として call scope に保持する。client-side decompression failure は DATA parser が compressed flag `1` を読んだ時点で、encoding が absent / identity / 実装未対応のいずれかを分類して初めて成立する。compressed flag `0` の message、または message を持たない trailers-only error では、未対応 `grpc-encoding` header は wire `grpc-status` を上書きしない。
- Why it matters: header-only rejection / payload破棄そのものは `origin/main` から存在するため、本PRが legal responseを新たに失敗化したわけではない。ただしPRはその既存の過剰な classification を、公式根拠とともに public code `UNIMPLEMENTED -> INTERNAL` へ変更し、code別 retry / error branchingを変えたうえで unit / PHPT / docsへ新しい正解として固定する。PR の目的である official alignment を完了したことにすると、producer側の不整合が見えなくなる。
- Recommended fix: header callback では `grpc_encoding` の保存だけを行い、`unsupported_response_encoding` / body discard / RST_STREAM は設定しない。unary / server-streaming 共通 DATA parser が compressed flag `1` を確定した時点で、(a) absent / identity encoding、(b) non-identity だが decompressor 未実装、(c)対応済み encoding を分けて分類する。少なくとも unary / server streaming で `gzip header + flag=0 + grpc-status:0 -> OK`、`gzip header + trailers-only non-OK -> wire status`、`gzip header + flag=1 -> INTERNAL` を fixture と public surface で固定し、official ext-grpc comparator も追加する。関連 docs も「未対応 `grpc-encoding` header」ではなく「未対応 encoding で実際に圧縮された message」に修正する。
- Inline comment anchor: `src/status_core.c:35`（PR added line）。この変更行で `compressed_response_seen` と header-level の `unsupported_response_encoding` を同じ official ruleへ畳んでいることを指摘する。補助 anchor は `tests/unit/test_status_core.c:57`。
- Fix summary: `grpc-encoding` callbackをalgorithm宣言の保存だけに戻し、Compressed-Flag=1を読んだDATA parserだけが `grpc_protocol_flag_compressed_message()` で `unsupported_response_encoding` / `compressed_response_seen` を確定するようにした。incremental / direct parserの両方へ同じ分類点を入れ、unary / server streamingで `gzip + flag=0 + status0 -> OK`、trailers-only non-OKのwire status維持、`gzip + flag=1 -> INTERNAL` をPHPTに固定した。SPECとprotocol classification docもper-message semanticsへ更新した。`
- Fix commit: `f5a2f751621cecbb447db7d89222df435fcf7849`
- Verification: `再レビューで src/transport.c のheader callbackと両DATA parser、src/status_core.c のpriorityを確認。./tools/test/check-c-unit.sh PASS (protocol_core / status_core / transport_core)、./tools/test/check-phpt.sh PASS (17/17)。PHPT 022でunary / server streamingのflag=0成功、trailers-only wire status、flag=1失敗とdetailsを確認。official ext-grpc comparatorは今回の再レビューでは再実行していない。`
- Notes: false-positive auditで、`unsupported_response_encoding` の header-level producer、body discard、wire statusより前のpriorityはすべて `origin/main` から存在することを再確認した。本指摘は変更行がその既存stateを `INTERNAL` として再固定した範囲だけを対象とする。`compressed_response_seen -> INTERNAL` 自体は client-side failure taxonomy として妥当。ext-grpc comparatorは既存 `dev-ext-grpc` source-built image上の runtime `grpc 1.80.0` / PHP `8.4.20` / `aarch64` で実施した観測であり、`ghcr.io/dkkoma/ext-grpc-artifacts` の明示artifact tagを使った再現ではない。修正後はheader観測だけでfailure flagやbody discardが成立しないため、当初のfalse classificationは解消した。

### REVIEW-20260710-002: missing-status clean close の ext-grpc / grpc-go compatibility policy を明示する必要がある

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2 / gRPC status taxonomy protocol adversary`
- Finding: PR が追加した `stream_closed && stream_error_code == NO_ERROR && http_status == 200` は、END_STREAMを運んだ frame が DATA か HEADERS かを区別せず、すべての clean close / `grpc-status` 欠落を `INTERNAL` にする。この uniform strict policy は「Trailersは必須」という protocol解釈からは成立し得るが、PRが根拠にする grpc-go は frame shapeで挙動が分かれ、repositoryの直接 comparatorである official ext-grpcも異なる。どの互換性を優先したかが未決定のまま「公式実装へ揃えた」と記録されている。
- Evidence: PR added `src/status_core.c:63-67` と `tests/unit/test_status_core.c:95-107` は terminal frame kindを持たず全 clean closeを `INTERNAL` とする。現在の grpc-goで `server closed the stream without sending trailers` / `INTERNAL` を作るのは [`handleData()` の DATA `f.StreamEnded()` branch](https://github.com/grpc/grpc-go/blob/master/internal/transport/http2_client.go) であり、PR issue `docs/issues/open/2026-07-08-status-taxonomy-official-alignment.md:29-34` が記す `operateHeaders()` ではない。同じ source の `operateHeaders()` は HEADERS END_STREAMで `grpc-status` が無ければ初期値 `codes.Unknown` を使う。official ext-grpc 1.80.0 との親レビュー実機比較では、(a) PR追加 fixture `poc/test-server/main.go:473-480` の message後 DATA END_STREAMは ext-grpcが unary response非null / `UNKNOWN(2)` / `Stream removed`、ce5872dが response null / `INTERNAL(13)` / new details、(b) headers-only/no-status と custom trailing-metadata/no-status は ext-grpcが `UNKNOWN`、ce5872dが `INTERNAL`、(c) trailing `grpc-message` onlyも ext-grpcが `UNKNOWN`、ce5872dが `INTERNAL` + peer supplied detailsだった。PR fixture / PHPTは(a)だけを固定するため、grpc-goのDATA/HEADERS差も ext-grpcのpayload差もdecision recordに現れない。
- Expected model: official implementationsが分かれる broken-response synthesisは、対象shape、status code、details、payload visibility、retryへの影響を含む compatibility decisionとして明示する。grpc-go exact alignmentなら DATA END_STREAM without trailersと HEADERS END_STREAM without statusを別 call classificationにする。ext-grpc drop-inまたはuniform strict policyを選ぶ場合も、その差をcurrent designへ残す。
- Why it matters: `UNKNOWN -> INTERNAL` だけでなく、DATA shapeでは official ext-grpcが返す decoded unary payloadまで nullになる。アプリの status別 retry、telemetry grouping、部分応答の扱いが変わる一方、PROTOCOL-HTTP2は壊れた responseからstatusをsynthesizeすることを要求しても、そのcode / payload visibilityを一意に定めていない。したがってcode bugと断定するより明示判断が必要である。
- Recommended fix: official ext-grpc comparatorを再現可能な verification recordとして残し、次のいずれかを明示acceptする。(a) drop-in priority: 観測shapeを `UNKNOWN` と ext-grpcのpayload visibilityへ合わせる。(b) grpc-go exact: DATA END_STREAMだけ `INTERNAL`、HEADERS END_STREAM without statusは `UNKNOWN` とし、terminal frame kindをcall-localに保持する。(c) uniform protocol strictness: 現predicateを維持するが、grpc-go / ext-grpc divergence、payload差、retry影響をSPEC / compatibility matrix / release noteに記録する。unary / server streamingで DATA、headers-only、custom trailing HEADERS、grpc-message-onlyを固定する。
- Inline comment anchor: `src/status_core.c:67`（PR added predicate）。「この1行はgrpc-goのDATA pathだけでなくHEADERS clean closeにも適用され、ext-grpcは追加fixture自体でもUNKNOWN/payload非nullだったため、どのpolicyを選ぶか明示してほしい」というinline commentにする。補助anchorは `poc/test-server/main.go:475`。
- Fix summary: compatibility policyとして **grpc-go exact** を明示採用した。call-localに `initial_headers_end_stream` と `trailing_headers_seen` を保持し、`:status 200` のclean closeかつstatus欠落を、DATA END_STREAMなら `INTERNAL`、initial / trailing HEADERS END_STREAMなら `UNKNOWN` と分類する。issueのdecision logにext-grpcとの差と採用理由を残し、headers-only、custom trailers、grpc-message-only、DATA no-trailersをunary / server streaming fixtureで固定した。`
- Fix commit: `f5a2f751621cecbb447db7d89222df435fcf7849`
- Verification: `再レビューで nghttp2 frame callback -> call-local flags -> grpc_lite_status_code_from_call() のlifecycleとpriorityを確認。./tools/test/check-c-unit.sh PASS、./tools/test/check-phpt.sh PASS (17/17)。PHPT 022でDATA END_STREAMはINTERNAL + fixed details、HEADERS END_STREAMはUNKNOWN、grpc-message-onlyはUNKNOWN + peer details、server streamingでは既受信messageを1件yieldすることを確認。nghttp2のHTTP messaging validationではtrailing HEADERSはEND_STREAM必須のため、NGHTTP2_HCAT_HEADERSをterminal observationとして扱う前提も成立する。`
- Notes: false-positive auditで、PR前から存在する non-terminal `grpc-status` / RST_STREAM precedenceは除外した。全 clean closeをINTERNALにする明示policyならterminal frame kindは不要なので、それ自体をcorrectness bugとはしない。ext-grpc comparatorは既存 `dev-ext-grpc` source-built image上の runtime `grpc 1.80.0` / PHP `8.4.20` / `aarch64` で実施した観測であり、`ghcr.io/dkkoma/ext-grpc-artifacts` の明示artifact tagを使った再現ではない。選択したgrpc-go exact policyとext-grpc divergenceはissueのdecision logに記録されたため、本Design DecisionはAcceptedとする。

### REVIEW-20260710-003: `grpc_call` field ownership mapに `trailing_headers_seen` が反映されていない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC status taxonomy protocol adversary`
- Finding: terminal frame kindをstatus taxonomyへ渡すcall-local stateとして `trailing_headers_seen` を追加したが、`grpc_call` の責務とlifetimeを明文化するfield ownership mapは新fieldを列挙していない。このmapは将来のfield分割やlifecycle変更時の設計基準と明記されているため、実装と設計資料がずれている。
- Evidence: `src/grpc_exchange_state.h:60` に `trailing_headers_seen` が追加され、`src/transport.c:2121-2127` がtrailing HEADERS観測時にsetし、`src/status_core.c:69-70` がmissing-trailers分類に使用する。一方、`docs/design/grpc-call-exchange-state.md:15` の「gRPC statusとvalidation flag」行は `initial_headers_end_stream` までしか列挙せず、同文書末尾は「この文書をfield ownership mapとして使う」としている。
- Expected model: `trailing_headers_seen` をgRPC status / stream lifecycleのcall-local fieldとしてownership mapに追加し、nghttp2 response frame callbackがsetし、status resolutionがDATA END_STREAMとHEADERS END_STREAMを区別するために読むことをlifetime説明へ反映する。
- Why it matters: runtime bugではないが、missing-trailers分類の成立条件からこのfieldを落とすと、将来のstruct整理やcallback責務変更でDATA / HEADERS差が再び潰される可能性がある。今回明示採用したgrpc-go exact policyの保守可能性に直接関係する。
- Recommended fix: `docs/design/grpc-call-exchange-state.md` のresponsibility mapへ `trailing_headers_seen` を追加し、`initial_headers_end_stream` と組でterminal frame shapeをcall lifetime中に保持することを短く記す。
- Inline comment anchor: `src/grpc_exchange_state.h:60`（PR added line）。新fieldを既存のfield ownership mapにも追加するよう指摘する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `pending: docs/design/grpc-call-exchange-state.md と src/grpc_exchange_state.h のfield map照合`
- Notes: `docs/design/protocol-classification-boundary.md` には新predicateが正しく反映済みであり、指摘はexchange-state ownership mapの同期漏れだけに限定する。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `1`
- Design Decision: `none`
