# 1xx informational response pass 13 finalization domain model review 2026-07-15

## Scope

- HEAD `712df8a` とcurrent working treeのpass-13未コミット差分
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/grpc_exchange_state.h`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `docs/design/grpc-call-exchange-state.md`
- `docs/verification/test-fixtures.md`
- `docs/verification/verification-matrix.md`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- `docs/reviews/issues/2026-07-15-1xx-adversarial-consolidated-pass13.md`

## Reviewer Role

- HTTP/2 / gRPC domain model・connection / stream / call lifecycle・informational HEADERS quarantine・fixture boundary reviewer

## Review Prompt Summary

- pass-13の3件、すなわちstatus-field経路に限定されていたincomplete-block quarantine、pre-quarantine sibling DATAを違反扱いしていたfixture、incomplete-block stateを反映していなかったcurrent-state docsの修正を再確認する。
- call-local failure taxonomyとHTTP/2 connection terminal actionの分離、target RST flush後のconnection lifecycle、sibling DATA観測境界、production / raw diagnostic / fixtureの責務境界を確認する。
- 実装変更は行わず、Blocker / High / Medium / Low / Design Decisionがnoneになることをrequired gateとして判定する。

## Issues

### REVIEW-20260715-001: failure-driven multiplex oracleの最終境界がdocs / pass-13 recordに反映されていない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model・fixture boundary reviewer`
- Finding: failure-driven correction後のfixtureは、32MiB WINDOW_UPDATEとmalformed HEADERSを50ms分割し、sibling DATAをtarget RST前に1件以上観測することをdiscriminator成立条件として必須にしたうえで、target RST後のsibling DATAを禁止する。PHPT 042もmultiplex全体のwall timeではなく、client trace上のtarget `RST_STREAM(CANCEL)`からtarget `rpc.end`までを500ms未満に固定し、target RST後にsibling DATA `frame_out`がないことを直接確認する。しかしcurrent fixture catalogとpass-13 Fix summary / Verificationはpre-quarantine DATAを「許容」「判定外」とだけ記し、このpositive setup conditionと最終client-side timing / DATA oracleを記録していない。
- Evidence: `poc/test-server/main.go`の`incompleteHeaderRstProbe.passed()`はsiblingがある場合に`p.siblingDataBeforeTargetRst`を必須とし、`multiplex-incomplete-entry-budget`はWINDOW_UPDATE後50ms待ってからincomplete budget HEADERSを送る。`tests/phpt/042-informational-1xx-adversarial.phpt`はtarget RST / target `rpc.end`の`monotonic_us`差を500,000us未満とし、target RST index後のsibling DATA `wire.frame_out`を拒否する。一方、`docs/verification/test-fixtures.md`の同control rowはpre-quarantine DATAを「許容」とだけ記し、pass-13 `REVIEW-20260715-002` Verificationは「pre-quarantine DATAは判定外」と記載する。open issueのpass-13 Progress / Verificationもfailure-driven correction後のoracleを記載していない。
- Expected model: test fixture catalogとreview / issue bookkeepingは、production lifecycleとtest discriminatorを分けて現在形で表す。production invariantはclassification後にsibling DATA providerをdeferすること、fixture setup invariantはmalformed HEADERS前の合法なsibling DATAをpositiveに観測すること、client-side oracleはtarget RST生成後のsibling DATA absenceとRST-to-completion bound、peer-side oracleはtarget RST実受信後のDATA absenceである。
- Why it matters: runtime behavior自体は正しくても、現在の記述ではfixtureが合法なpre-quarantine DATAを単に無視するように読め、failure-driven correctionで追加されたdiscriminatorとwall-time assertion削除の理由を将来の変更が失う。test setup backpressureとterminal quarantine graceを再び混同しやすくなる。
- Recommended fix: `docs/verification/test-fixtures.md`のmultiplex rowへpre-target-RST sibling DATAが必須のpositive setup oracleであることを追記する。pass-13 `REVIEW-20260715-002`のFix summary / Verificationとopen issueのpass-13 Progress / Verificationを、32MiB WINDOW_UPDATE → 50ms split、pre-target-RST DATA必須、peer / client双方のpost-RST DATA absence、client traceのtarget RST→`rpc.end` <500ms、end-to-end wall timeを使わない理由に合わせて更新する。verification matrixは少なくとも「CANCEL後DATA absence」がclient trace / peer markerの両方で固定されることを明確にする。
- Fix summary: `docs/verification/test-fixtures.md`のmultiplex control rowへ32MiB WINDOW_UPDATE、50ms split、pre-target-RST sibling DATA必須、client trace / peer marker双方のpost-target-RST DATA absence、target RST→`rpc.end` 500ms未満を追記した。`docs/verification/verification-matrix.md`はclient traceとpeer markerのoracleを区別し、pass-13 recordのfinding 1〜3のFix summary / Verificationとdated Progress / Verification、open issueのdated Progress / Verificationもfailure-driven correction後の最終境界と4 suite結果へ更新した。
- Fix commit: `pending`
- Verification: `poc/test-server/main.go`のpositive pre-RST setup / negative post-RST peer oracle、PHPT 042のpost-RST client trace / RST-to-completion oracleと、fixture catalog、verification matrix、pass-13 record、open issueを静的に相互照合した。pass-13 record / open issueにはtest-server rebuild/recreate、PHPT 29/29、C unit 4/4群、PHPUnit 31 tests / 116 assertions、C static analysis findings noneのdated PASS結果が記録されている。`
- Notes: production transport / fixture implementation / PHPT oracleへの追加修正findingではなく、pass-13 finding 2 / 3のfinal bookkeeping consistencyに限定する。

## Prior Finding Recheck

### REVIEW-20260715-001: END_HEADERS未完了blockのterminal quarantineがstatus-field経路に限定されている

- Status: `Fixed`（adequate）
- Fix commit: `pending`
- Verification: `grpc_protocol_apply_response_header_terminal_action()`が、caller-ownedのprimary failure taxonomy / RST codeと、stream / connection terminal actionを分離している。END_STREAMなしtrailingはbegin callback、informational END_STREAMは`:status` callback、normal / invalid fieldのwire budget超過はbudget ownerから同helperへ入り、END_HEADERSなしの場合だけcall-local `response_header_block_incomplete`をsetしてconnectionをclose-after-pending-flushへ遷移させる。productionは既存の固定grace flush、全DATA provider defer、flush後dead化へ接続し、unary / server streamingのtarget taxonomyと既存siblingの`UNAVAILABLE`を分離する。raw diagnosticは同じclassificationを読み、one-shot fdだけをnonblocking化するため、persistent connection ownershipを取り込んでいない。PHPT 042 / 043はinformational END_STREAM、non-terminal trailing、wire entry budgetの3 triggerについてunary / server streaming / diagnosticの有限終了、primary taxonomy、expected RST、fresh follow-upを固定している。

### REVIEW-20260715-002: multiplex fixtureがclient-side quarantine開始前のsibling DATAも違反扱いする

- Status: `Fixed`（adequate）
- Fix commit: `pending`
- Verification: fixtureの`incompleteHeaderRstProbe`は、probe作成時点ではなくpeerがcaller-selected target RSTを受信した時点をapplication DATA違反境界にする。`multiplex-incomplete-entry-budget`は32MiBのsibling / connection WINDOW_UPDATEとmalformed target HEADERSを50ms分割し、clientがquarantine前に通常sendしたsibling DATAをpositive setup conditionとして必須にする一方、target CANCEL受信後のsibling DATAをfailureにする。PHPT 042はclient traceでもtarget RST後のsibling DATA `frame_out`不在とtarget RSTからtarget `rpc.end`まで500ms未満を確認するため、合法なpre-quarantine backpressureをterminal graceのwall timeへ混ぜない。target RSTの実受信、post-RST DATA absence、connectionを跨ぐfresh follow-upはauthority-keyed storeで分離され、fixture stateはtest-server process / connection handler内だけにあり、production transportへtest-only責務を追加していない。

### REVIEW-20260715-003: 新しいincomplete-block stateとfollow-up controlがownership / fixture mapに反映されていない

- Status: `Fixed`（adequate）
- Fix commit: `pending`
- Verification: field ownership mapは`response_header_block_incomplete`のshared producer、production connection quarantine consumer、raw diagnostic nonblocking consumer、production call / diagnostic iteration reset lifetimeを現在形で記載する。fixture catalogは3種のfragmented control、budget-trigger multiplex、authority-keyed cross-connection markerについてexpected target RST、pre-target-RST DATAのpositive setup、client trace / peer markerのpost-target-RST DATA boundary、target RST→`rpc.end` bound、最大3秒wait / one-shot consumeを記載し、verification matrixもunary / server streaming / diagnostic coverageと一致する。

## Domain Gate Checks

- Domain objects: incomplete response header blockのcall-local classificationは`grpc_call`、HPACK decoderを再利用不能にするterminal lifecycleは`h2_connection`、RST codeとprimary gRPC statusはtriggerごとのprotocol classificationが所有し、scopeを混同していない。
- Naming / responsibility: shared helper名はresponse-header terminal actionを表し、failure taxonomyを決めずにtarget streamのRST submitとincomplete-block時のconnection transitionだけを所有する。fixture内部型もstatus-field専用名からgeneric `incompleteHeaderRstProbe`へ更新されている。
- Lifecycle: END_HEADERS未完了時は新規stream admissionを止め、quarantine後のapplication DATAをdeferし、target control frameを固定grace内でbest-effort flushした後にconnectionをdead化する。admitted streamを完走させるGOAWAY drainingとは別のstate transitionとして維持される。
- Error taxonomy: informational END_STREAM / non-terminal trailingは`INTERNAL` + `PROTOCOL_ERROR`、wire budget超過は`RESOURCE_EXHAUSTED` + `CANCEL`、non-terminal initial status fieldは`UNKNOWN` + `CANCEL`を維持し、connection terminal actionがprimary call resultを上書きしない。
- Unary / server streaming: 同じHTTP/2 stream-level callback / connection transitionを共有し、server streamingだけは既にdelivery済みのmessage countを維持して後続pullを`UNAVAILABLE`で終える。connection-global stateへcall kind固有責務を追加していない。
- Production / diagnostic / fixture boundary: productionはpersistent `h2_connection`、raw diagnosticはdisposable fd、fixtureはpeer-observed frame markerをそれぞれ所有し、共有するのはcall-local classificationとprotocol callback actionだけである。
- Documentation: ownership mapのproducer / consumer / reset、authority-keyed markerのwait-consume、failure-driven multiplex oracleのpositive setup / client trace / peer marker / timing boundaryがcurrent implementationと一致する。pass-13 recordとopen issueにはdated Progress / Verificationと最終4 suite結果が記録されている。

## Scope Triage

- fixture control文字列`require-prior-incomplete-status-cancel`はhistorical nameのままだが、current catalog・generic probe type・error message・expected-RST selectionはいずれもstatus-field / CANCEL専用ではない実際のcontractを明示している。test-only selectorのrenameは今回の3 findingのcorrectness / lifecycle gateを変えないためfindingにしない。
- `response_header_block_incomplete`は単なるHEADERS受信中stateではなく、END_HEADERS未完了block内でterminal failureが確定したcall-local classificationである。field名より狭い意味はownership mapとshared helper commentで限定され、connection terminal stateそのものは`close_after_pending_flush`が所有するためscope collapseではない。
- peer-side markerはclient内部のquarantine transition時刻を直接観測せず、target RSTというwire-visible boundaryを使う。分割したWINDOW_UPDATE / malformed HEADERSとpre-RST DATA必須条件によりpre-quarantine sendを識別し、post-RST DATAだけを禁止するため、fixtureがproduction implementation detailを所有していない。

## Verification

- current diff、adjacent callback / send / recv loop、status priority、unary / server streaming connection-dead handlingを静的照合
- fixtureのWINDOW_UPDATE → delay → incomplete HEADERS → target RST → idle drain → authority-keyed follow-up sequenceを静的照合
- PHPT 042 / 043のtaxonomy、finite completion、RST code / attribution、target RST→`rpc.end` bound、post-RST sibling DATA absence、sibling lifecycle、fresh connection oracleを静的照合
- ownership map、fixture catalog、verification matrix、pass-13 recordのFix summary / Verificationを実装と相互確認
- runtime suiteはこのreview-only subtaskでは未実行（実装agentのfinal verification laneに委譲）

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none（1件Fixed）
- Design Decision: none
