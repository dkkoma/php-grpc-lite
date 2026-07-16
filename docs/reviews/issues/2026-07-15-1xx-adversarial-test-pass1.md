# Informational 1xx response adversarial test / fixture review 2026-07-15

## Scope

- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `src/grpc_exchange_state.h`
- `src/transport.c`
- `src/diagnostic/bench.c`
- `tools/test/check-c-unit.sh`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- `docs/verification/test-fixtures.md`
- `docs/verification/verification-matrix.md`
- `docs/verification/compatibility-control-checklist.md`

## Reviewer Role

- test / fixture adversary

## Review Prompt Summary

- commit `20c2dc0` の response header-block phase modelについて、Go fixtureの103 field送出と後続header mutation、unary / server streaming PHPTのClose Criteria・識別力・message cardinality、post-1xx固有shape、C unit gate、fixture / verification docsとの整合をread-onlyで確認した。Go 1.26 / `x/net/http2` の1xx `writeHeader()` はlive header mapを渡す場合にframe write完了までblockするため、fixtureはpolluted 103を確定してからfieldを削除し、clean final 200を送る。したがってpollution testはvacuousでもfinalへのleakでもなく、fixture catalogの説明と一致する。新規assertionはpre-changeまたは却下実装 `375c3dd` を識別し、常に自明に通るassertionは見つからなかった。

## Issues

### REVIEW-20260715-001: post-1xx固有のphase edgeがend-to-endで未検証

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `test / fixture adversary`
- Finding: 新規PHPTは103を1回送った後にfinal initial HEADERS、1 DATA message、別blockの`grpc-status` trailerへ進むshapeだけを成功経路として検証する。semantic phase model固有の (1) 複数informational blockの反復、(2) 103直後のfinal block自体が`grpc-status`を持つtrailers-only response、(3) post-1xx final `HCAT_HEADERS`でinitial response validationが失敗するnegative pathは未検証である。
- Evidence: `tests/phpt/022-error-and-http-validation.phpt:95-142,251-310` の全1xx caseは `poc/test-server/main.go:427-441` の単一`WriteHeader(103)`を使う。成功caseは同file `:442-449` または `:553-563` がfinal 200 HEADERS、空message DATA、`grpc-status: 0` trailing HEADERSを送る。`src/transport.c:2232-2248,2343-2354` には、post-1xxの`FINAL_INITIAL` block内で`grpc-status`を観測してmetadataをtrailingへ再分類し、同じHEADERSのEND_STREAMを`initial_headers_end_stream`へ記録する専用経路があるが、`x-bench-early-hints=1` と既存 `x-bench-grpc-response=headers-only` の組合せはテストされない。またfinal側のinvalid `content-type` / non-200 `:status`との組合せもない。
- Expected model: final response未観測中は任意個の1xx blockを `AWAITING_STATUS -> INFORMATIONAL -> NONE` と反復して完全に隔離し、最初のnon-1xx blockはraw category、DATA有無、`grpc-status`同居の有無にかかわらず `FINAL_INITIAL` としてmetadata ownershipとinitial validationを適用する。trailers-only final HEADERS + END_STREAMも既存gRPC semanticsどおり解決する。
- Why it matters: 現行PHPTをすべて通したまま、2個目の103をtrailing扱いする回帰、post-1xx trailers-only errorをinvalid initial statusとして拒否する回帰、またはinformational隔離をfinal blockまで広げてcontent-type / HTTP status validationを抑止する回帰を導入できる。trailers-onlyはbodyを持たないimmediate gRPC errorの通常shapeであり、今回のphase modelが追加した専用分岐である。
- Recommended fix: 既存controlでunary / server streaming双方に (a) `x-bench-early-hints=1 + x-bench-grpc-response=headers-only + x-bench-grpc-status=5`（0 message、`NOT_FOUND`、空details）、(b) `x-bench-early-hints=1 + x-bench-grpc-status=0 + x-bench-content-type=text/plain`（`UNKNOWN`、final content-type由来details）を追加する。fixtureへ103 repeat countを追加し、pollution付き103を2回送った後もclean final responseへ進むcaseを少なくとも1件固定する。
- Fix summary: port 50054 controlを拡張し、`x-bench-early-hints-count=2`のpollution付き複数103、103後のTrailers-Only status 5、103後のinvalid final content-typeをunary / server streamingへ追加した。raw `:50071` にはpost-1xx malformed edgeとbench用sequenceを追加した。
- Fix commit: `pending`
- Verification: `tests/phpt/022-error-and-http-validation.phpt` がunary / server streamingのTrailers-Only `NOT_FOUND`、invalid final content-type `UNKNOWN`、pollution付き2回103後のclean final responseを確認。test-server再build / force-recreate PASS、`./tools/test/check-phpt.sh` 28/28 PASS。
- Notes: Go `net/http` でvalidな複数103は50054、libraryが送出を拒むmalformed wire shapeは専用raw fixture 50071に分けた。fixture catalogとpreflightを50071まで更新した。

### REVIEW-20260715-002: server-streaming 1xx testの`messageCount(10)`がfixtureに無視される

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `test / fixture adversary`
- Finding: server-streamingのmetadata ownership / pollution caseはrequestへ`messageCount=10`を設定する一方でyield count 1を期待するが、port 50054のraw HTTP handlerは`BenchRequest`をdecodeせず、該当success branchがwireへ送るgRPC messageは常に1件である。したがってこれは1xx後に9件がdropした実証ではなくdead inputを含むfixture limitationであり、post-1xxのmulti-message継続性を検証していない。
- Evidence: `tests/phpt/022-error-and-http-validation.phpt:264-274,279-293` は`setMessageCount(10)`の後にcount 1をassertする。`poc/test-server/main.go:425-578` の`serveNonGrpcH2C()`はrequest body / protobuf / method pathを解釈せず、`x-bench-observe-authority` branch `:442-449` は`grpcFrame(0, nil)`を1回だけ書く。generic `x-bench-grpc-status` branch `:553-563`も同様に1回だけである。
- Expected model: PHPTのrequest cardinalityとfixtureが送るresponse cardinalityを一致させる。1-messageのheader phase testならrequestも1件とし、server-streamingでpost-1xx DATA deliveryの継続性を主張する場合はfixtureが複数messageを明示送出してその全件をassertする。
- Why it matters: 現行testでもresponse bodyの全面discardは検出できるが、最初のmessageだけyieldして2件目以降を落とす回帰は検出できない。また`10 -> 1`という見た目がfixture仕様を知らないreviewerにmessage lossと誤解され、testのoracleを弱くする。
- Recommended fix: phase / metadataだけを見るcaseは`setMessageCount(1)`へ揃える。加えて既存 `x-bench-grpc-response=two-messages` と `x-bench-early-hints=1` を組み合わせるserver-streaming caseを追加し、2件yield、status OK、final metadata ownershipをassertする。
- Fix summary: port 50054を使うserver-streaming PHPTのrequestをfixtureの1 response messageと合う `messageCount(1)` へ揃えた。さらに `x-bench-early-hints=1` + `x-bench-grpc-response=two-messages` を追加し、post-1xx DATA deliveryが2 messageとも継続することを固定した。
- Fix commit: `pending`
- Verification: PHPT 022のearly-hints two-message caseがyield count 2、status OKを確認。その他の50054 phase / validation caseはrequest 1対fixture response 1に揃い、全PHPT 28/28 PASS。
- Notes: 10→1のdead inputは削除し、single-messageのownership検証とmulti-message継続性検証のoracleを分けた。

### REVIEW-20260715-003: phase transitionが共通pure helper / C unitで固定されていない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `test / fixture adversary`
- Finding: `AWAITING_STATUS / INFORMATIONAL / FINAL_INITIAL / TRAILING` のstate transitionはnghttp2やZendに依存しない判定として切り出せるが、production callbackとbench diagnostic callbackへ重複実装され、C unitはenumも遷移も参照しない。PHPTはproduction callbackのsingle-103だけを通すため、bench側のparityと反復call resetはcommitが記す検証結果では守られない。
- Evidence: phase enum/stateは `src/grpc_exchange_state.h:27-33,78-79`。productionは `src/transport.c:2181-2193,2209-2231,2343-2375`、diagnosticは `src/diagnostic/bench.c:292-340,453-486,1364-1375` に同型のbegin/status/end/reset処理を別々に持つ。`tools/test/check-c-unit.sh:17-29` は`protocol_core` / `status_core` / `transport_core`の3 suiteだけを実行し、`tests/unit/`にはphase enumや`final_response_headers_seen`を検証するassertionがない。`docs/verification/verification-matrix.md:71` はC unitをpure protocol/status/transport helper boundaryのgateと定義する一方、新規1xx row `:29` のMain verificationはPHPTだけである。
- Expected model: response header-block phaseのbegin/status/end transitionをproduction / diagnosticが共有するpure ownerへ置き、direct final、single/multiple informational、final initial、trailingのtransition tableとcall resetをC unitで固定する。wire-level PHPTはPHP-visible semanticsを担当し、C unitはstate machineの全edgeを担当する。
- Why it matters: productionとdiagnosticの片側だけが変更されても既存gateが成功し得る。特にbench batchは同じ`grpc_call`をiteration間でreuseするため、`final_response_headers_seen` resetの欠落やphase実装のdriftが通常PHPTでは観測されない。issue Verificationの「C unit 3/3 PASS」は既存suiteの回帰なしを示すだけで、新しいphase model自体のcoverageにはなっていない。
- Recommended fix: phase begin、`:status` classification、block end/resetをnghttp2 / Zend非依存の共通helperへ抽出し、C unitへ `200 -> trailing`、`103 -> 200 -> trailing`、`103 -> 103 -> 200 -> trailing`、call reset後の再実行をtable-drivenで追加する。production / diagnostic callbackは共通helperだけでphaseを更新する。
- Fix summary: nghttp2 / Zend非依存の `src/response_header_phase.{h,c}` を追加し、begin / `:status` / end / resetに加え、phase×END_STREAMのstatus commit predicateとparse非依存metadata role predicateをproduction / diagnosticで共有した。table-driven C unitはdirect final、single/repeated 1xx、trailing、call reuse reset、commit/ownership truth tableを覆う。
- Fix commit: `pending`
- Verification: `./tools/test/check-c-unit.sh` 4/4 PASS（`response_header_phase` suiteを含む）。production / benchの両方がshared transition / status-validity / metadata-role helperだけを呼ぶことをpass-2 domain-model reviewで確認。verification matrixの1xx row / C-unit gateも新suiteへ更新。
- Notes: 初回修正でphase transitionのみ共有しEND_STREAM validityが重複していたため、pass-2でpure predicateまで追加抽出した。これによりprotocol-003のproduction / diagnostic parityを構造的に維持する。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none` (`1 fixed`)
- Low: `none` (`2 fixed`)
- Design Decision: `none`
