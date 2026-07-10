# PR #28 status taxonomy C safety adversarial review 2026-07-10

## Scope

- `src/status_core.c`
- `src/transport.c`
- `src/grpc_exchange_state.h`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `src/diagnostic/bench.c`
- `tests/unit/test_status_core.c`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/Integration/CompressionTest.php`
- `poc/test-server/main.go`

## Reviewer Role

- C extension / nghttp2 state-machine adversarial reviewer

## Review Prompt Summary

- PR #28 の current HEAD `093b808809420616dfb990417607584ea4dd209a` を、第三パス対象`0f1cc090a9ecf04ecc9b7f4b78b719101b21456b`からのrevert差分に限定して第四パス再レビューした。不完全な1xx成功経路の`expect_final_response` field/callback、early-hints fixture/PHPT、current design記述が完全に除去され、元のstatus-taxonomy fixで必要な`trailing_headers_seen`のEND_STREAM gateだけが残ることを確認した。unary/server streaming/transparent retry/benchの初期化とreuseも再監査し、一般的な1xx対応はscope外とした。

## Issues

### REVIEW-20260710-001: 既存の過剰なencoding flagを`INTERNAL`として再固定している

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `C extension / nghttp2 state-machine adversarial reviewer`
- Finding: `unsupported_response_encoding`は実際にcompressed messageの展開に失敗したことではなく、non-identity `grpc-encoding` headerを観測しただけで立つ既存flagである。`origin/main`でもこのflagはwire `grpc_status`より先に`UNIMPLEMENTED`へ解決されていたため、header-only rejectionとbody discard自体はPRが導入した問題ではない。しかしPRは変更行で同じ過剰なstateを`INTERNAL`へ変更し、`grpc_status = OK`より優先する期待値もC unitで`INTERNAL`へ更新している。したがって「公式実装へ揃える」変更として、producerの意味を検証せず既存誤分類を新しいpublic statusで再固定している。
- Evidence: 修正前`ce5872d`の`src/transport.c:2044-2052`は`origin/main`と同一であり、header callbackだけで`unsupported_response_encoding = true`と`discard_response_body = true`を設定していた。`origin/main:src/status_core.c:33-34`はこのflagをwire statusより先に`UNIMPLEMENTED`へ解決し、`ce5872d:src/status_core.c:35-36`は同じpriorityで`INTERNAL`へ変更していた。最小C stateは同commitの`tests/unit/test_status_core.c:57`にあり、`unsupported_response_encoding = true; grpc_status = GRPC_STATUS_OK`の期待値をPRが`UNIMPLEMENTED`から`INTERNAL`へ変更していた。PHPTの`x-bench-grpc-encoding: gzip` fixtureもcompressed flag `0`のmessageを返す既存shapeの期待値だけを`INTERNAL`へ変更していた。
- Expected model: status taxonomyへ渡すclassification flagは「headerでalgorithm名を観測した」状態と「compressed flag `1`のmessageを、未対応algorithmのため展開できない」状態を区別する。client-side decompression failureだけがwire `grpc-status`を上書きする`INTERNAL`となり、compressed flag `0`のmessageではencoding headerだけでpayload/statusを破棄しない。
- Why it matters: legal responseを失敗扱いする基礎問題は既存だが、このPRはアプリから見えるcodeを`UNIMPLEMENTED`から`INTERNAL`へ変え、code別retry/error branchingを変える。また変更後のC unitとPHPTが過剰なstateの`INTERNAL`優先を回帰期待値として固定するため、PR説明のofficial alignmentを満たしたように見えてproducer側の不整合を残す。
- Recommended fix: header callbackではencoding名の観測だけを保持し、DATA parserがcompressed flag `1`を確認した時点で未対応encoding failureを立てる。最低限、unary/server streamingで`grpc-encoding: gzip + compressed flag 0 + grpc-status: 0`がwire status/payloadを保持し、`gzip + compressed flag 1`だけが`INTERNAL`になるfixtureを追加する。別issueへ分ける場合も、本PRがheader-only rejectionを新規導入したとは扱わず、今回変更するstatus mappingが既存のover-broad flagを再固定する点を明記する。
- Fix summary: `on_header_callback()`は`grpc-encoding`の値だけをcall-owned `zend_string`へ保存し、header観測だけではfailure/body discardを立てないようになった。direct / non-direct DATA parserの双方がCompressed-Flag=`1`を確定した時点で`grpc_protocol_flag_compressed_message()`を呼び、non-identity encoding宣言ありなら`unsupported_response_encoding`、宣言なし/identityなら`compressed_response_seen`へ分類する。gzip宣言 + flag=`0` + wire OK、gzip宣言 + trailers-only non-OK、gzip宣言 + flag=`1`をunary/server streamingのPHPTで固定した。
- Fix commit: `f5a2f751621cecbb447db7d89222df435fcf7849`
- Verification: `./tools/test/check-c-unit.sh` pass; `./tools/test/check-c-static-analysis.sh` pass; `./tools/test/check-phpt.sh` 17/17 pass。producer/consumer追跡でもflag=`0`経路はpayload decodeとwire statusを保持し、flag=`1`だけがlocal INTERNALをwire statusより優先することを確認した。
- Notes: inline commentを`src/status_core.c:35`へ置くことは、この行がuser-visible codeを変更し、変更済みunit expectationを固定するため妥当。ただし「header観測でflag/body discardを立てる処理をPRが追加した」という表現は誤りであり、HighではなくMediumとした。C memory safetyの問題ではない。

### REVIEW-20260710-002: `NGHTTP2_HCAT_HEADERS`をterminal trailersと同一視している

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `C extension / nghttp2 state-machine adversarial reviewer`
- Finding: 新設された`trailing_headers_seen`は「HEADERS END_STREAMで閉じた」ことを表すclassification fieldとして`status_core.c`から読まれるが、producerは`NGHTTP2_HCAT_HEADERS`を受けた時点でframe flagsを見ずにtrueへする。このcategoryはterminal trailersを意味せず、nghttp2ではfirst response以外のgeneric HEADERS（1xx後のfinal response headersを含む）である。したがってnon-terminal HEADERSの後にDATA END_STREAMでclean closeすると、実際のterminal frameはDATAなのに`trailing_headers_seen == true`が新しいmissing-trailers predicateを抑止し、`INTERNAL`ではなく`UNKNOWN`へ落ちる。
- Evidence: `src/transport.c:2121-2127`は`frame->headers.cat == NGHTTP2_HCAT_HEADERS`だけでfieldを立て、`NGHTTP2_FLAG_END_STREAM`を確認しない。`src/status_core.c:69-70`はそのfieldをterminal HEADERSの代理として否定条件に使う。`tests/unit/test_status_core.c:106-110`も`trailing_headers_seen = true`を直接与えるだけで、producerがEND_STREAMを伴ったことを固定していない。[nghttp2公式enum説明](https://nghttp2.org/documentation/enums.html#c.nghttp2_headers_category)は`NGHTTP2_HCAT_HEADERS`を他categoryに該当しないHEADERSとし、non-final response後のfinal response HEADERSもこのcategoryになると明記する。PRがalignment根拠にした[grpc-go `operateHeaders()`](https://github.com/grpc/grpc-go/blob/master/internal/transport/http2_client.go)は、initial response後のsecond HEADERSで`endStream == false`なら`INTERNAL`（`a HEADERS frame cannot appear in the middle of a stream`）としてstreamを閉じる。
- Expected model: call fieldはframe categoryではなくterminal eventを表す。少なくとも`NGHTTP2_FLAG_END_STREAM`を伴うHEADERSだけを`terminal_headers_end_stream`（または同等のfield）として記録し、DATA END_STREAMとの区別に使う。initial response後のnon-terminal second HEADERSはgrpc-go exact policyなら独立したmalformed/protocol-error classificationにし、1xxを扱う場合はinformational/final response lifecycleと区別する。
- Why it matters: malformed peerまたはinformational responseを含む合法HTTP/2 sequenceが一度generic HEADERSを通るだけで、新設したDATA-vs-HEADERS taxonomyがcall終了まで汚染される。PRの判断ログが宣言する「grpc-go exact」と異なるuser-visible code/detailsになり、field名・comment・unit testが誤ったproducer invariantを正しいものとして固定する。memory corruptionではないが、adversarial server responseに対するprotocol classification bypassである。
- Recommended fix: `trailing_headers_seen`をEND_STREAM確認付きのterminal fieldへ変更し、`on_frame_recv_callback()`で`(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0`の場合だけ立てる。raw h2 fixtureで少なくとも (a) trailing HEADERS + END_STREAM + statusなし → UNKNOWN、(b) non-terminal second HEADERSの後にDATA END_STREAM → INTERNAL（またはより早いmalformed INTERNAL）をunary/server streaming双方で固定する。必要なら1xx後のfinal response HEADERSを別fixtureにしてcategory単独ではtrailersを表さないことも固定する。
- Inline comment anchor: 修正前`f5a2f75:src/transport.c:2127`。current HEADのfixは`src/transport.c:2129-2130`。
- Fix summary: `on_frame_recv_callback()`はgeneric `NGHTTP2_HCAT_HEADERS`だけでは`trailing_headers_seen`を立てず、`NGHTTP2_FLAG_END_STREAM`付きの場合だけterminal HEADERSとして記録する。`375c3dd`で一時追加された1xx fixture/成功経路は`093b808`で別issueへrevertされたが、前回指摘の本体であるEND_STREAM gateはそのまま維持された。
- Fix commit: `375c3ddd73a040f99de5b6fb3f217b36020d3344`（END_STREAM gate）、`093b808809420616dfb990417607584ea4dd209a`（scope-expanded 1xx path revert）
- Verification: `src/transport.c:2121-2131 producer gate audit at 093b808; ./tools/test/check-c-unit.sh pass; ./tools/test/check-c-static-analysis.sh pass; ./tools/test/check-phpt.sh 17/17 pass。PHPT 022のDATA END_STREAM/no trailers -> INTERNALとtrailing HEADERS END_STREAM/no status -> UNKNOWNはunary/server streamingで維持される。`
- Notes: initial response後のnon-terminal`grpc-status`受理全体はPR前から存在するため、本指摘は新設fieldがEND_STREAMなしのgeneric HEADERSをterminal eventとして記録し、新設predicateを誤って抑止する範囲に限定する。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Verification

- `grpc_call` のproduction初期化を確認した。unaryはattemptごとのstack objectを`memset`し、server streamingはattemptごとのresource stateをzero-initializeしたうえで、両方とも`grpc_status = -1`、`http_status = -1`を明示する。`stream_error_code`のzero値は`NGHTTP2_NO_ERROR`と同値だが、新しいmissing-trailers判定は`stream_closed`でもgateされるため、未開始callを誤分類しない。
- `on_frame_recv_callback()`のinbound `RST_STREAM`は`stream_reset_seen`とwire error codeを保持し、`on_stream_close_callback()`は最終close codeを保存してactive stream登録を外す。status taxonomyではwire status、GOAWAY refusal、RST_STREAMがclean close判定より先に解決されるため、`RST_STREAM(NO_ERROR)`をmissing trailersへ誤分類しない。
- GOAWAY refused pathは`stream_error_code = NGHTTP2_REFUSED_STREAM`、`stream_refused_seen = true`を設定してからstreamをunregisterする。新しい`stream_error_code == NGHTTP2_NO_ERROR`条件とは衝突しない。
- compression / unsupported encodingのdetails分岐は既存の`zend_string`をcopyまたは新規allocateして返し、unary/server streaming result assemblyがcopy後に元をreleaseする既存ownershipを維持する。追加行にNULL dereference、UAF、長さ計算、integer overflowはない。
- bench専用batch pathは同じ`grpc_call`をiteration間で再利用するが、productionの`resolve_grpc_call_status()`を使用せず、各iterationで`stream_closed`、`grpc_status`、`http_status`、`stream_error_code`、`compressed_response_seen`をresetする。今回のproduction taxonomy変更によるstale-state伝播はない。
- `expect_final_response`は`grpc_call`、production callback、fixture/PHPTから除去され、runtime symbol/referenceは残っていない。bool fieldだけのrevertなのでcleanup/ownership追加処理も不要であり、field削除後のlayoutを全translation unitが同じheaderからcompileする内部structのためABI mismatchはない。
- 残る`trailing_headers_seen`はproduction unaryのattempt-local stack objectとserver streamingのattempt-local `ecalloc` resourceでzero initializeされ、transparent retryもfresh call/resourceを作る。bench callbackはこのfieldを更新せずstatus taxonomyも呼ばないため、bench reuseにstale-state伝播はない。
- `grpc_encoding`はduplicate headerで旧`zend_string`をreleaseしてから新規allocateし、call cleanupで1回releaseする。Compressed-Flag helperは有効なcall-owned stringをreadするだけで、direct/non-direct parserともflag設定後にbody discard/RSTへ遷移する。追加経路にNULL dereference、UAF、double free、length overflowは見つからなかった。
- compression details selectionはfresh production callで2つのclassification flagがともにfalseから始まり、Compressed-Flag=`1`でnon-identity `grpc_encoding`があれば`unsupported_response_encoding`だけ、無ければ`compressed_response_seen`だけを立てるため、`unsupported grpc-encoding: <name>`とgeneric compressed-message detailsの分岐は決定的である。non-empty peer `grpc_message`を全local synthesisより優先する既存policyは今回変更されておらず、このPR固有のfindingにはしない。missing-trailers detailsはcodeが`INTERNAL`になったDATA-close shapeだけで新設文言へ到達し、HEADERS-closeの`UNKNOWN`では空またはpeer `grpc-message`を維持する。
- `./tools/test/check-c-static-analysis.sh` (`093b808`): pass。
- `./tools/test/check-c-unit.sh` (`093b808`): pass（`protocol_core` / `status_core` / `transport_core`）。
- `./tools/test/check-phpt.sh` (`093b808`): 17/17 pass。compression、DATA END_STREAM、headers-only/trailing HEADERSのunary/server streaming経路を含む。revert対象の103成功経路はfixture/testともcurrent suiteから除去済み。
- GitHub Native QA (`ce5872d`): Static analysis、NTS PHPT + C coverage、Crash/UB check、ZTS PHPTはいずれもsuccess。

### `trailing_headers_seen` initialization / reset audit

- Production unary: `grpc_lite_unary_call_perform_core_on_connection()`はattemptごとにstack上の`grpc_call`全体を`memset(0)`する。wrapperのtransparent retry loopは同関数を再呼び出して別のfresh objectを作るため、attempt間でfieldは継承されない。
- Production server streaming: `server_streaming_call_open_resource()`はattemptごとに`server_streaming_call_state`を`ecalloc`し、さらにembedded `state->call`全体を`memset(0)`する。transparent retryは旧resourceをdestructして同open関数からfresh resourceを作る。diagnostic server-streaming entry pointも同じopen関数をattempt 0で使う。
- Raw diagnostic batch: `grpc_lite_bench_unary_batch()`のstack `grpc_call`はbatch開始時に全体zero-initされ、iteration間でreuseされる。iteration resetには`trailing_headers_seen`が無いが、専用`bench_on_frame_recv_callback()` / `bench_on_header_callback()`はこのproduction-only fieldを一度もsetせず、raw benchは`grpc_lite_status_code_from_call()` / `resolve_grpc_call_status()`も呼ばない。したがって現行callback setではstale-state findingはない。ただし将来bench callbackをproduction callbackへ統合する場合はfull per-iteration resetが必要になる。
- Tests: C status unitのmacroも各assertで`grpc_call`全体をzero-initする。fixture間のstate leakはない。

### Fourth-pass revert completeness audit

- Runtime C: `src/grpc_exchange_state.h`から`expect_final_response`を削除し、`on_frame_recv_callback()`は通常の`HCAT_RESPONSE` validationへ戻った。current runtime sourceに同fieldのproducer/consumerは無い。
- Fixture/tests: `poc/test-server/main.go`の`x-bench-early-hints` controlとPHPT 022の4ケース（unary 2 / streaming 2）は削除された。current fixture inventoryからもcontrolが除去され、別issueだけが将来の再実装scopeとして記録する。
- Final retained change: `trailing_headers_seen`のstruct field、status coreの`!trailing_headers_seen` predicate、`on_frame_recv_callback()`の`NGHTTP2_FLAG_END_STREAM` gate、current ownership docは互いに整合する。fieldをsetするproduction callbackは1箇所、consumerはstatus taxonomyの1箇所で、revertによるdead/stale branchはない。
- Historical docs: status-taxonomy issue/review record内の`expect_final_response`言及は追加・却下・revertの判断履歴でありcurrent designの主張ではない。current design docはfieldを除去済み。

### nghttp2 callback-order audit

- [nghttp2 session receive contract](https://nghttp2.org/documentation/nghttp2_session_recv.html)ではvalid inbound HEADERSは、各`on_header_callback()`、header block完了後の`on_frame_recv_callback()`、そのframeがstreamを閉じる場合の`on_stream_close_callback()`の順で通知される。従ってterminal HEADERSでは`grpc_status` / `grpc_message` / metadataが先に保存され、次にEND_STREAM-gated frame markerが保存され、最後に`stream_closed` / close error codeが保存される。close callback内ではstream user dataが有効で、unregister後のstatus resolutionはunary stack/resourceを直接参照するためUAFはない。
- inbound DATAは`on_data_chunk_recv_callback()`の後に`on_frame_recv_callback()`、END_STREAMなら`on_stream_close_callback()`となる。現実装はDATAのEND_STREAMをpositive fieldへ保存せず、「terminal HEADERS fieldが無いclean close」として推論する。`375c3dd`のEND_STREAM gateによりnon-terminal generic HEADERSはこの推論を汚染しなくなり、REVIEW-20260710-002は解消した。
- inbound RST_STREAMは`on_frame_recv_callback()`で`stream_reset_seen` / wire error codeを保存してからclose callbackへ進む。GOAWAY refused pathはactive callへrefused/closed stateを直接保存してunregisterする。いずれもmissing-trailers predicateより高priorityのstateを持ち、今回のfield初期化/reset問題はない。

### Third/fourth-pass cross-review deduplication

- `375c3dd`の`expect_final_response`がframe callbackにだけ導入され、先行するheader callbackがfinal response fieldsをtrailing metadataへ保存する問題は[protocol adversarial reviewのREVIEW-20260710-004](2026-07-10-pr28-status-taxonomy-protocol-adversary.md)へ集約した。`093b808`は同field/fixture/testsをPR #28からrevertし、独立1xx issueへ戻したため、C側にもproblematic phase stateは残らない。
- C safety固有には、revert後のdangling field reference、未初期化、attempt/iteration間stale state、callback後UAF、string ownership破壊、status priorityの新たな不整合は見つからなかった。

## Second-pass False-positive Audit

- `unsupported_response_encoding`: header観測によるflag設定、body discard、wire `grpc_status`より前の解決priorityはすべて`origin/main`から存在する。PRが新規導入したのは`UNIMPLEMENTED -> INTERNAL`のcode変更と、その期待値更新である。したがって既存bug全体をPR regressionとして説明するのはfalse positiveだが、変更されたpublic codeとtest lock-inに限定したinline指摘は成立する。
- Minimal C-level proof: `tests/unit/test_status_core.c:57`の1 stateで十分に「flagがvalid wire OKより優先し、PRによって結果がINTERNALへ変わった」ことを示す。ただし`grpc_call`の最終stateだけではflagがheader-only observation由来かcompressed DATA由来かを表現できないため、producerの誤りそのものは`on_header_callback()`の静的追跡またはwire-level fixtureが必要である。
- Missing trailers: `ce5872d`時点ではuniform strict policyならterminal frame origin fieldは不要であり、frame区別欠如だけをcorrectness findingにはしなかった。`f5a2f75`でgrpc-go exactを選びterminal fieldを新設したため、producerがEND_STREAMを確認することはpolicy実現の必須invariantになった。`NGHTTP2_HCAT_HEADERS` categoryだけではそのinvariantを満たさない点をREVIEW-20260710-002として新規指摘した。initial response後のnon-terminal `grpc-status`受理全体は既存問題なのでscope外のままにした。

## Official ext-grpc Comparator Caveat

- Provenance: comparatorは`compose.yaml`の`dev-ext-grpc` serviceと既存local image `php-grpc-lite-dev-ext-grpc`を使用した。`docker/Dockerfile.ext-grpc`はversion未固定の`pecl install grpc`でsource buildするため、AGENTS.mdが通常比較に要求する`ghcr.io/dkkoma/ext-grpc-artifacts:<version>-php<version>-<distro>-<arch>-<profile>`から取得したbinaryではない。実行時probeでは`grpc 1.80.0`、`PHP 8.4.20 CLI`、`Linux aarch64`、local image id `sha256:2f5fdb20b2047b776cfb6e3e2a03c92adfa14e48cce90f7ee9494e2d43597037`を確認した。repositoryの`grpc/grpc` wrapperも`composer.lock`で`1.80.0`である。
- Method: parent reviewerが追跡外の一時Go h2c probeをhost port `50080`で起動し、`host.docker.internal:50080`へofficial containerとlite containerから同じwrapper callを送った。`grpc-encoding: gzip + Compressed-Flag=0 + grpc-status:0`はext-grpcがnon-null response / `OK(0)` / empty details、lite HEADがnull / `INTERNAL(13)` / `unsupported grpc-encoding: gzip`だった。probe sourceはレビュー後に削除済みであり、このexact variantは現時点でrepository内のdurable fixtureではない。tracked 50054の既存gzip fixture（flag `0`だがgrpc-status欠落）でもext-grpcはpayloadをdeserializeし、最終statusだけをmissing-status由来の`UNKNOWN / Stream removed`にしたため、headerだけではpayloadを捨てないという核心は独立に観測できる。
- Reliability judgment: gzip flag `0`の結果はprotocol定義とofficial implementation sourceにも一致し、compiler profileやartifact packagingで変わる性質ではないため、GitHubのcorrectness reviewに用いてよい。ただし表現は「official PECL ext-grpc **1.80.0** on PHP 8.4.20/aarch64で観測」と限定し、canonical artifactを使った、または全ext-grpc versionで同一とは書かない。future rebuildではunversioned Dockerfileが別versionを取得し得るため、再現時はruntime `phpversion('grpc')`を必ず記録し、可能ならcanonical tag `1.80.0-php8.4-trixie-arm64-pecl`で再実行する。
- Missing-status judgment: tracked `no-trailers` fixtureに対するext-grpc `1.80.0`のunary `BenchReply / UNKNOWN(2) / Stream removed`、server streaming `1 message / UNKNOWN(2) / Stream removed`は再現可能なempirical observationとして十分である。一方、broken-responseのsynthesized code/detailsはversion・implementation policy差があり得るため、これは「INTERNALが誤り」というnormative証拠ではなく、grpc-go strictnessとext-grpc 1.80.0 drop-in behaviorが分かれるDesign Decisionの証拠としてだけ使う。
- Target-version caveat: `composer.lock`のwrapperは1.80.0だが、`composer.json`のdependency-resolution platformは`ext-grpc 1.62.0`、既存diagnostic image workflowのdefault comparatorは1.58.0であり、repository全体で唯一のofficial extension baselineが1.80.0へ固定されているわけではない。1.58/1.62互換まで主張または要求する場合は各versionのartifactで別途確認する。一つのcurrent official versionがPRのgenericな「official ext-grpc alignment」と異なるという反証には1.80.0観測で足りる。
