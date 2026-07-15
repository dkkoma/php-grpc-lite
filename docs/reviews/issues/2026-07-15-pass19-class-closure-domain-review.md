# pass-19 class-closure domain model review 2026-07-15

## Scope

- HEAD `b17201d` とcurrent working treeのpass-19未コミット差分
- `src/response_header_phase.[ch]`
- `src/transport.[ch]`
- `src/transport_core.[ch]`
- `src/diagnostic/bench.c`
- `poc/test-server/main.go`
- `tests/unit/test_response_header_phase.c`
- `tests/unit/test_transport_core.c`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- 関連SPEC、design、code-reading、fixture、verification資料
- `docs/reviews/issues/2026-07-15-1xx-adversarial-consolidated-pass19.md`

## Reviewer Role

- HTTP/2 / gRPC domain model gate reviewer（pass-19 class closure）

## Review Prompt Summary

- nghttp2のnormal / recoverably-invalid / strict-rejected response header notificationが、field class × semantic phaseのclosed routeとshared wire-header budget / incomplete-header terminal actionへ収束するか確認した。
- call-local taxonomy、stream RST ownership、connection-global HPACK terminal lifecycle、unary / server streaming、production / raw diagnosticの責務境界を確認した。
- exhaustive C unit、strict-invalid pseudo-headerとuppercase regular-name raw fixture、PHPTのstatus / RST / fresh connection / diagnostic finite-finish oracleを確認した。

## Issues

### Blocker

- none

### High

- none

### Medium

- none

### Low

#### REVIEW-20260715-001: connection lifecycle ownerのコメントがRST ownership拡張前のCANCEL専用モデルを残す

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model gate reviewer（pass-19 class closure）`
- Finding: incomplete inbound HPACK blockのconnection quarantine ownerは、pass-19でclient-owned `CANCEL`だけでなくclient-owned / nghttp2-owned `PROTOCOL_ERROR`を含むpending control frame全般を扱う。一方、実際にconnection stateを更新するhelperのコメントはtarget `CANCEL`のflushまでI/Oを維持すると説明したままであり、RST ownerから分離した現在のlifecycle contractと一致しない。
- Evidence: `src/transport.c` の `mark_connection_close_after_pending_flush()` は `draining` / `close_after_pending_flush` を設定する共有connection lifecycle ownerだが、コメントは「target CANCEL is flushed」と限定する。pass-19の `grpc_protocol_mark_response_header_terminal_action()` はnghttp2-owned strict rejectionでも同helperを呼び、`docs/design/grpc-call-exchange-state.md` のrouting tableはpending control frameとRST ownership分離を現在のmodelとして記述する。
- Expected model: connection-terminal quarantineのownerは、call taxonomyやRST code / submit ownerに依存せず、END_HEADERS未完了blockで既にqueue済みのcontrol frameへbounded flush opportunityを与えた後に全ownerをterminal化する、と実装近傍でも一貫して表現する。
- Why it matters: 今後このhelperを変更する際、コメントをcontractとして読むとCANCELだけを特別扱いし、nghttp2-owned `PROTOCOL_ERROR`のflushまたはconnection terminal化を誤って外す可能性がある。今回閉じたproducer classを再びRST ownership別に分岐させる誘因になる。
- Recommended fix: `mark_connection_close_after_pending_flush()` のコメントをtarget CANCELではなく、already-owned / pending control frame（client-ownedまたはnghttp2-owned RSTを含む）のbounded flushとconnection-terminal quarantineとして更新する。コード変更は不要。
- Fix summary: `mark_connection_close_after_pending_flush()` のcontract commentをCANCEL専用表現からbounded pending-control flushへ一般化した。併せてowning designのstate mapを`grpc_protocol_mark_response_header_terminal_action()`がincomplete classificationを所有し、client-owned RSTはapply helperがmark + submit、nghttp2-owned rejectionはmarkのみを行う現在の責務へ揃えた。invalid-frame observerは`call->metadata_too_large || lib_error_code == NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`でproducer-owned taxonomyを保存する。これによりinvalid-header budget stopをnghttp2が`HTTP_HEADER`として通知するruntimeでも`RESOURCE_EXHAUSTED` / selected `CANCEL`を維持し、normal field-route TEMPORALの`INTERNAL` / selected `PROTOCOL_ERROR`も同じproducer-owned branchへ入る。
- Fix commit: `pending`
- Verification: `src/transport.cのconnection lifecycle comment、production / diagnostic invalid-frame observer、docs/design/grpc-call-exchange-state.mdのownership / TEMPORAL routing、unknown classを含む全5 phaseのC unit tableを再照合。invalid-header budget stopのHTTP_HEADER通知でもsticky metadata_too_largeがprimary taxonomyを所有し、strict rejectionはmetadata_too_large未設定時にREJECTED routeへ進むことを確認。git diff --check PASS。./tools/test/check-c-unit.sh PASS（4/4 suites）。全runtime / static suite PASS。`
- Notes: `none`

### Design Decision

- none

## Domain Gate Checks

- Field classification / phase closure: `STATUS` / `REGULAR` / `INVALID_REGULAR` / `REJECTED`は`NONE` / `AWAITING_STATUS` / `INFORMATIONAL` / `FINAL_INITIAL` / `TRAILING`の全phaseにrouteを持ち、unknown enum値はdefault terminal protocol errorへfail closedする。reported normal / invalid fieldはroute前にshared wire budgetを通り、strict rejectionはname / value非公開のためbudget計上不能であることを明示したうえで`REJECTED` routeへ入る。
- Taxonomy / RST ownership: budget超過は`metadata_too_large` / `RESOURCE_EXHAUSTED` / client-selected `CANCEL`を維持する。field-route rejectionは`response_header_protocol_error` / `INTERNAL`となる。strict HTTP messaging rejectionではnghttp2-owned RSTを重複submitせず、`grpc_protocol_mark_response_header_terminal_action()`だけでincomplete lifecycleへ接続する。
- Call / stream / connection lifecycle: `response_header_block_incomplete`はcall-local classificationであり、productionではconnectionのadmission停止とclose-after-pending-flush、diagnosticではone-shot fdのnonblocking化を各scopeのconsumerが担当する。GOAWAY drainingとincomplete HPACK terminal quarantineを混同していない。
- Unary / server streaming: production callbackとstatus taxonomyは両call kindで共有される。PHPT 042は各新controlをunary / server streamingへ適用し、有限`INTERNAL`、exact `PROTOCOL_ERROR`、connection quarantine、fresh follow-upを確認する構造である。
- Production / diagnostic boundary: raw diagnosticはproductionのbudget / field classifier / route / terminal markを共有し、persistent connection lifecycleを持たずfd modeだけをdiagnostic固有consumerとして扱う。PHPT 043はstrict rejectionでinvalid-header callbackを迂回することと実fd `O_NONBLOCK`を固定する。
- Fixture / oracle: strict-invalid pseudo-header `:foo: v` とuppercase regular name `X-Bad: v` はexact HPACK literalを直接送るため、encoder正規化を受けない。completed 103の後のEND_STREAM / END_HEADERSなしHEADERSとCONTINUATION省略によりfindingのconnection-global incomplete stateを再現する。
- Public / internal boundary: 新しいfield class / route / transport action helperはC extension private header内に留まり、`Grpc\\` public surfaceやchannel optionを増やしていない。bench観測値もbench-enabled diagnostic resultに限定される。
- Re-review disposition: connection lifecycle commentはRST code / submit ownerに依存しないbounded pending-control flushへ修正された。owning designのstate mapとTEMPORAL row、production / diagnosticの`metadata_too_large || TEMPORAL` producer-owned判定、unknown class × 全5 phase unitもcurrent implementationとruntime observationに一致し、新規findingはない。

## Verification

- current diff、adjacent callback registration、phase reset、status priority、connection admission / dead transitionを静的照合
- production / diagnosticのnormal・invalid・strict-reject callbackからbudget、field route、terminal mark、RST ownershipまでを静的照合
- raw fixtureとPHPT 042 / 043のunary / server streaming / diagnostic oracleを静的照合
- `git diff --check` PASS
- `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）
- Low修正後の再レビューで `git diff --check` PASS、`./tools/test/check-c-unit.sh` PASS（4/4 suites）
- final logic delta後の `git diff --check` PASS、`./tools/test/check-c-unit.sh` PASS（4/4 suites）
- test-server image rebuild / force recreate PASS
- `./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）
- `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）
- `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled、exit 0）

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
