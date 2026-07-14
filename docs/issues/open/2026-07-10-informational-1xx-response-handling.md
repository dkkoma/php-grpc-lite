# 1xx (informational) 応答の HEADERS を final response として誤処理しない

- Status: Open
- Created: 2026-07-10
- Branch: codex/issue-informational-1xx-response-handling
- Owner: Claude

## Background

[2026-07-08-status-taxonomy-official-alignment](2026-07-08-status-taxonomy-official-alignment.md)（PR #28）の敵対的再レビュー [Medium] 指摘（`NGHTTP2_HCAT_HEADERS` ≠ terminal trailers）への対応中に一度実装したが、第三パスレビュー（protocol-adversary `REVIEW-20260710-004`）で「frame-end 判定だけの不完全な 1xx 成功経路」と指摘され、PR #28 からは revert して本 issue の別 PR スコープとした。

nghttp2 の category 契約では、最初の response HEADERS だけが `NGHTTP2_HCAT_RESPONSE` で、1xx (informational) の場合は後続の non-final block と final response HEADERS がすべて `NGHTTP2_HCAT_HEADERS` で届く。着手前の実装は:

1. `HCAT_RESPONSE`（= 1xx block）に対して content-type validation を実行するため、content-type を持たない 1xx で `invalid_content_type` が誤発火し、1xx を挟む応答は失敗する（既知の制限として許容中）。
2. `on_header_callback()` は raw category だけで trailing / initial を決めて即時に call state へ反映するため、frame 完了後にしか分からない「この block は 1xx / final / trailing のどれか」という semantic phase を知らない。

## 却下された初回実装（参考）

PR #28 の commit `375c3dd` で `expect_final_response` フラグによる frame-end 判定を実装したが、以下の理由で revert（レビュー実測 probe 付き）:

- 1xx 後の final response HEADERS のフィールドが trailing metadata として保存される（`x-bench-observe-authority` + 103 併用で `x-bench-authority` が `getMetadata()` から `getTrailingMetadata()` へ移動）。
- 1xx block 内の `content-type` / `grpc-status` / `grpc-message` / `grpc-encoding` が final response の validation / status / compression 分類を汚染しうる（RFC 8297 §2: 103 のフィールドは final response の処理に影響してはならない）。
- frame-end の `on_frame_recv_callback()` では、既に行われた header callback の metadata 追加や semantic field 更新を修復できない。

## Goals

- response header block に call-local な semantic phase（informational / final initial / trailing）を持たせ、`on_begin_headers_callback()` 等で **header callback 時点までに** phase を確定する。
- informational block のフィールドを隔離する（metadata / validation / status state に反映しない）。
- 1xx 後の final response `HCAT_HEADERS` を initial response headers として処理する（metadata は initial 側、validation も initial 相当）。
- unary / server streaming で status / details に加えて **metadata ownership（initial / trailing の帰属）** と 1xx field isolation をテストで固定する。

## Non-Goals

- 100-continue 等、1xx への能動的な応答動作。受動的に無視して final response を待つのみ。

## Plan

- `on_begin_headers_callback()`（または frame header 到達時点）で block phase を決定: 未 final なら `:status` を見る前に informational 候補として開始し、`:status` 確定時に phase を確定する方式を検討（`:status` は block 先頭で届く保証があるため header callback 内での確定も可）。
- fixture: `x-bench-early-hints=1`（103 先行送出、他 control と併用可、PR #28 から revert したものを復活）に加え、1xx block に semantic field（content-type / grpc-status 等）を含める汚染ケースを追加。
- PHPT: 1xx + no-trailers → INTERNAL / 1xx + status0 → OK / metadata ownership（`x-bench-observe-authority` 併用で initial 帰属を assert）/ 1xx field isolation を unary / streaming で固定。

## Progress

- 2026-07-10: PR #28 内で `expect_final_response` による frame-end 判定を一度実装（`375c3dd`）→ 第三パスレビュー `REVIEW-20260710-004` の指摘（上記「却下された初回実装」）を受けて PR #28 から revert。terminal frame 判別（`trailing_headers_seen` の END_STREAM ゲート）のみ PR #28 に残した。
- 2026-07-15: `grpc_call` に response header-block semantic phase（`AWAITING_STATUS` / `INFORMATIONAL` / `FINAL_INITIAL` / `TRAILING`）と final response 観測stateを追加した。`on_begin_headers_callback()` でblock開始phaseを決め、先頭の`:status` callbackで informational / final initial を確定する。1xx fieldはmetadata count/bytesを含むcall stateへ反映せず、1xx後に `HCAT_HEADERS` で届くfinal responseへinitial metadata ownershipとinitial content-type validationを適用する。
- 2026-07-15: `x-bench-early-hints=1` fixtureを復活し、103だけにinvalid `content-type`、`grpc-status`、`grpc-message`、`grpc-encoding`、custom metadataを載せる `x-bench-early-hints-pollution=1` を追加した。PHPT 022でunary / server streaming双方のmissing trailers、status 0、metadata ownership、informational field isolationを固定した。
- 2026-07-15: bench-enabled diagnostic nghttp2 callbackにも同じphase transitionと反復callごとのstate resetを適用し、production / diagnosticのどちらでもinformational fieldがmetadata / status観測へ混ざらないようにした。
- 2026-07-15: `docs/SPEC.md`、response exchange / transport / protocol classification設計、code-reading guide、fixture / verification資料をsemantic phase modelへ更新した。
- 2026-07-15: HTTP/2 / gRPC domain model reviewを実施し、Blocker / High / Medium / Low / Design Decisionの指摘はいずれもnone。記録は `docs/reviews/issues/2026-07-15-informational-1xx-response-domain-model-review.md`。
- 2026-07-15: pass-1 adversarial review 8件を受け、response phaseとstatus commit validityのproduction / diagnostic共有pure helper、`response_header_protocol_error` taxonomy、END_STREAMなしtrailing blockのquarantine、nghttp2 invalid-frame / outbound protocol-RST観測を追加した。HTTP messaging rejectionは先行 `grpc-status: 0` とHTTP status未観測fallbackより優先し `INTERNAL` とする。
- 2026-07-15: semantic metadata ownershipとwire header work budgetを分離し、pseudo-headerとinformational field、複数1xx blockをoverflow-safeなcall-local entry/byte counterへ累積するようにした。budget超過は `RESOURCE_EXHAUSTED` + stream-local cancelとし、同一connectionの後続RPC成功もfixtureで固定した。
- 2026-07-15: raw h2c fixture `:50071` とPHPT 042/043を追加。END_STREAMなしtrailer、103+END_STREAM、103→missing-status HEADERS、103→DATA、informational entry/byte budget、invalid status前後のmetadata ownership、bench false-successをunary / server streamingまたはbench entrypointで固定した。PHPT 022には複数103、post-1xx Trailers-Only / invalid content-type、two-message streamingを追加し、request cardinalityを1に揃えた。
- 2026-07-15: pass-3 adversarial reviewの重複12指摘を6テーマへ整理し、invalid regular header callbackを含むwire header budget、diagnostic default limit / iteration reset / success gate、`grpc-status-details-bin` terminal gate、foreign pushed-stream RST ownershipを修正した。wire budget算術は`transport_core`のpure helper、call classificationと`RST_STREAM(CANCEL)`はproduction / diagnostic共有ownerへ置き、semantic phase責務と分離した。
- 2026-07-15: raw fixtureのresource oracleを対象streamのexact `RST_STREAM(CANCEL)`受信でgateし、pseudo / regular field classを片側実装では通せない65-block / 237-byte境界へ変更した。silent-ignoreされるNUL-bearing invalid regular field、diagnostic default 64KiB、non-terminal status-details、2-iteration reset、foreign pushed-streamのcontrolsを追加した。
- 2026-07-15: PHPT 043のvalid positive controlにより、raw benchが`call.connection == NULL`のままproduction `connection_send()`を呼び、全batchを送信前に失敗させていた既存不整合を検出した。raw diagnosticのsocket ownershipに合わせてbench-local fd sendへ修正し、positive 2 iterationsとnegative controlsの双方を通した。
- 2026-07-15: pass-4 domain model gateで、invalid-header budget超過後のTEMPORAL callback stopを0へ変換していたMedium 1件を検出した。production / diagnostic双方でTEMPORALを伝播し、invalid-frame observerは既確定の`metadata_too_large`をprotocol errorで上書きしないよう修正した。再レビュー後のBlocker / High / Medium / Low / Design Decisionはすべてnone。

## Verification

- 2026-07-15: `docker compose build test-server` / `docker compose up -d --force-recreate test-server` PASS。wire probeでpollution fieldを持つ103の後に、cleanなfinal 200 initial HEADERSと `grpc-status: 0` trailerが届くことを確認。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（26/26 tests、failed 0、skipped 0、warned 0）。PHPT 022のunary / server streaming 1xx status、metadata ownership、field isolationを含む。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / status_core / transport_core、3/3 suites）。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions、failures 0、errors 0、skipped 0）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-4 HTTP/2 / gRPC domain model review PASS（初回Medium 1件を修正後、Blocker / High / Medium / Low / Design Decision: none）。記録は `docs/reviews/issues/2026-07-15-1xx-pass3-fix-domain-model-pass4.md`。
- 2026-07-15: domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。
- 2026-07-15: pass-1 adversarial fix後にtest-server imageをrebuildし `docker compose up -d --force-recreate test-server` PASS。raw `:50071` のexact malformed/resource/ownership/bench sequenceと、`:50054` のvalid repeated-103 / post-1xx edgeを送出した。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（28/28 tests、failed 0、skipped 0、warned 0）。PHPT 042がunary / server streamingのmalformed sequence、wire header budgetとsame-connection follow-up、invalid-status metadata ownership、PHPT 043がbench `ok=0` / `failed=1` を含む。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）。response phase transition、call reset、status commit / metadata role truth tableを含む。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-2 HTTP/2 / gRPC domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。記録は `docs/reviews/issues/2026-07-15-1xx-fix-domain-model-pass2.md`。
- 2026-07-15: pass-3 fix後に `docker compose build test-server` と `docker compose up -d --force-recreate test-server` を実行し、raw fixture imageのrebuild / restart PASS。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（28/28 tests、failed 0、skipped 0、warned 0）。PHPT 042でvalid / invalid regular fieldのentry・byte budget、field-class境界、exact CANCEL、same-connection reuse、status-details gateを、PHPT 043でvalid 2-iteration reset、entry / default-byte budget、status-details、foreign pushed-stream ownershipを確認した。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）。wire header budgetの128-entry境界、exact byte上限、加算overflowを含む。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。

## Decision Log

- 2026-07-10: 記録のみの issue 分割（コードは PR #28 同梱のまま）としたが、第三パスレビューで不完全性が実証されたため、コードごと本 issue の別 PR スコープへ変更。
- 2026-07-15: nghttp2がHEADERS rejectionとDATA-after-1xxで異なるcallbackを通るため、HTTP response messaging violationはDATA framingの `malformed_response_frame` と混ぜず `response_header_protocol_error` とした。HEADERSは `on_invalid_frame_recv_callback()`、DATA-after-1xxはnghttp2が生成するoutbound `RST_STREAM(PROTOCOL_ERROR)` observerで捕捉し、重複RSTはsubmitしない。
- 2026-07-15: final-response後のTrailing HEADERSはbegin callback時点でEND_STREAM flagを読めるため、END_STREAMなしblockのstatus/metadataはその場でquarantineする。nghttp2のblock全体validation後に別の違反が分かった場合はprotocol-error markerが先行statusより優先するため、header field全体のstaging allocationは追加しない。
- 2026-07-15: semantic metadata map用counterをinformational fieldで増やすとownershipが混ざるため、独立したwire header entry/byte counterを導入。configured metadata hard limitと128-entry limitを全decoded response fieldのwork budgetとしても使う。
- 2026-07-15: phase begin / `:status` / end / resetとstatusのEND_STREAM commit predicateをnghttp2 / Zend非依存のpure helperに置き、production / bench diagnosticの構造的parityをC unit transition / truth tableで守る。

## Close Criteria

- 1xx を挟む応答で metadata ownership / field isolation / status が正しいことを固定する PHPT が unary / server streaming で通る。
- 既存スイート（C unit / PHPT / PHPUnit）に回帰がない。
