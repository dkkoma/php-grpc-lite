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

- PR #28 の current HEAD `ce5872d` を `origin/main...HEAD` で確認し、初期値とsentinel、attempt/reuse間のstale state、memory ownership、分岐優先順位と到達可能性、NULL/UAF/overflow、nghttp2 callback ordering、END_STREAM / RST_STREAM、production / fixture境界を敵対的にレビューした。既存の2件のPRコメント（current docs更新、missing-trailers PHP surface coverage）は対応済みとして重複対象から除外した。

## Issues

### REVIEW-20260710-001: 既存の過剰なencoding flagを`INTERNAL`として再固定している

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `C extension / nghttp2 state-machine adversarial reviewer`
- Finding: `unsupported_response_encoding`は実際にcompressed messageの展開に失敗したことではなく、non-identity `grpc-encoding` headerを観測しただけで立つ既存flagである。`origin/main`でもこのflagはwire `grpc_status`より先に`UNIMPLEMENTED`へ解決されていたため、header-only rejectionとbody discard自体はPRが導入した問題ではない。しかしPRは変更行で同じ過剰なstateを`INTERNAL`へ変更し、`grpc_status = OK`より優先する期待値もC unitで`INTERNAL`へ更新している。したがって「公式実装へ揃える」変更として、producerの意味を検証せず既存誤分類を新しいpublic statusで再固定している。
- Evidence: `src/transport.c:2044-2052`は`origin/main`とHEADで同一であり、header callbackだけで`unsupported_response_encoding = true`と`discard_response_body = true`を設定する。`origin/main:src/status_core.c:33-34`はこのflagをwire statusより先に`UNIMPLEMENTED`へ解決し、HEADの`src/status_core.c:35-36`は同じpriorityで`INTERNAL`へ変更する。最小C stateは`tests/unit/test_status_core.c:57`に既にあり、`unsupported_response_encoding = true; grpc_status = GRPC_STATUS_OK`の期待値をPRが`UNIMPLEMENTED`から`INTERNAL`へ変更している。PHPTの`x-bench-grpc-encoding: gzip` fixtureもcompressed flag `0`のmessageを返す既存shapeの期待値だけを`INTERNAL`へ変更している。
- Expected model: status taxonomyへ渡すclassification flagは「headerでalgorithm名を観測した」状態と「compressed flag `1`のmessageを、未対応algorithmのため展開できない」状態を区別する。client-side decompression failureだけがwire `grpc-status`を上書きする`INTERNAL`となり、compressed flag `0`のmessageではencoding headerだけでpayload/statusを破棄しない。
- Why it matters: legal responseを失敗扱いする基礎問題は既存だが、このPRはアプリから見えるcodeを`UNIMPLEMENTED`から`INTERNAL`へ変え、code別retry/error branchingを変える。また変更後のC unitとPHPTが過剰なstateの`INTERNAL`優先を回帰期待値として固定するため、PR説明のofficial alignmentを満たしたように見えてproducer側の不整合を残す。
- Recommended fix: header callbackではencoding名の観測だけを保持し、DATA parserがcompressed flag `1`を確認した時点で未対応encoding failureを立てる。最低限、unary/server streamingで`grpc-encoding: gzip + compressed flag 0 + grpc-status: 0`がwire status/payloadを保持し、`gzip + compressed flag 1`だけが`INTERNAL`になるfixtureを追加する。別issueへ分ける場合も、本PRがheader-only rejectionを新規導入したとは扱わず、今回変更するstatus mappingが既存のover-broad flagを再固定する点を明記する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `origin/main...HEAD state/priority diff audit; tests/unit/test_status_core.c:57 is the minimal C-level state proof`
- Notes: inline commentを`src/status_core.c:35`へ置くことは、この行がuser-visible codeを変更し、変更済みunit expectationを固定するため妥当。ただし「header観測でflag/body discardを立てる処理をPRが追加した」という表現は誤りであり、HighではなくMediumとした。C memory safetyの問題ではない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `1`
- Low: `none`
- Design Decision: `none`

## Verification

- `grpc_call` のproduction初期化を確認した。unaryはattemptごとのstack objectを`memset`し、server streamingはattemptごとのresource stateをzero-initializeしたうえで、両方とも`grpc_status = -1`、`http_status = -1`を明示する。`stream_error_code`のzero値は`NGHTTP2_NO_ERROR`と同値だが、新しいmissing-trailers判定は`stream_closed`でもgateされるため、未開始callを誤分類しない。
- `on_frame_recv_callback()`のinbound `RST_STREAM`は`stream_reset_seen`とwire error codeを保持し、`on_stream_close_callback()`は最終close codeを保存してactive stream登録を外す。status taxonomyではwire status、GOAWAY refusal、RST_STREAMがclean close判定より先に解決されるため、`RST_STREAM(NO_ERROR)`をmissing trailersへ誤分類しない。
- GOAWAY refused pathは`stream_error_code = NGHTTP2_REFUSED_STREAM`、`stream_refused_seen = true`を設定してからstreamをunregisterする。新しい`stream_error_code == NGHTTP2_NO_ERROR`条件とは衝突しない。
- compression / unsupported encodingのdetails分岐は既存の`zend_string`をcopyまたは新規allocateして返し、unary/server streaming result assemblyがcopy後に元をreleaseする既存ownershipを維持する。追加行にNULL dereference、UAF、長さ計算、integer overflowはない。
- bench専用batch pathは同じ`grpc_call`をiteration間で再利用するが、productionの`resolve_grpc_call_status()`を使用せず、各iterationで`stream_closed`、`grpc_status`、`http_status`、`stream_error_code`、`compressed_response_seen`をresetする。今回のproduction taxonomy変更によるstale-state伝播はない。
- `./tools/test/check-c-static-analysis.sh`: pass。
- `./tools/test/check-c-unit.sh`: pass（`protocol_core` / `status_core` / `transport_core`）。
- `./tools/test/check-phpt.sh`: 17/17 pass。変更されたcompressionとclean END_STREAM without `grpc-status`のunary/server streaming経路を含む。
- GitHub Native QA (`ce5872d`): Static analysis、NTS PHPT + C coverage、Crash/UB check、ZTS PHPTはいずれもsuccess。

## Second-pass False-positive Audit

- `unsupported_response_encoding`: header観測によるflag設定、body discard、wire `grpc_status`より前の解決priorityはすべて`origin/main`から存在する。PRが新規導入したのは`UNIMPLEMENTED -> INTERNAL`のcode変更と、その期待値更新である。したがって既存bug全体をPR regressionとして説明するのはfalse positiveだが、変更されたpublic codeとtest lock-inに限定したinline指摘は成立する。
- Minimal C-level proof: `tests/unit/test_status_core.c:57`の1 stateで十分に「flagがvalid wire OKより優先し、PRによって結果がINTERNALへ変わった」ことを示す。ただし`grpc_call`の最終stateだけではflagがheader-only observation由来かcompressed DATA由来かを表現できないため、producerの誤りそのものは`on_header_callback()`の静的追跡またはwire-level fixtureが必要である。
- Missing trailers: `stream_closed`と`stream_error_code == NGHTTP2_NO_ERROR`はnormal closeを表すが、END_STREAMを運んだframe種別を保持しない。とはいえPRが明示するpolicyが「HTTP 200の全clean closeでvalid `grpc-status`が無ければINTERNAL」である限り、frame originはそのpredicateに不要であり、C state modelingだけからcorrectness findingにはしない。DATA/initial HEADERS/trailing HEADERSでgrpc-go/ext-grpc互換を分けるならterminal frame origin fieldが必要になるが、それはcompatibility policyのDesign Decisionである。non-terminal `grpc-status`をfinal扱いするpriorityも既存であり、このPRのnew missing-trailers branchへ混ぜてHigh findingにしない。

## Official ext-grpc Comparator Caveat

- Provenance: comparatorは`compose.yaml`の`dev-ext-grpc` serviceと既存local image `php-grpc-lite-dev-ext-grpc`を使用した。`docker/Dockerfile.ext-grpc`はversion未固定の`pecl install grpc`でsource buildするため、AGENTS.mdが通常比較に要求する`ghcr.io/dkkoma/ext-grpc-artifacts:<version>-php<version>-<distro>-<arch>-<profile>`から取得したbinaryではない。実行時probeでは`grpc 1.80.0`、`PHP 8.4.20 CLI`、`Linux aarch64`、local image id `sha256:2f5fdb20b2047b776cfb6e3e2a03c92adfa14e48cce90f7ee9494e2d43597037`を確認した。repositoryの`grpc/grpc` wrapperも`composer.lock`で`1.80.0`である。
- Method: parent reviewerが追跡外の一時Go h2c probeをhost port `50080`で起動し、`host.docker.internal:50080`へofficial containerとlite containerから同じwrapper callを送った。`grpc-encoding: gzip + Compressed-Flag=0 + grpc-status:0`はext-grpcがnon-null response / `OK(0)` / empty details、lite HEADがnull / `INTERNAL(13)` / `unsupported grpc-encoding: gzip`だった。probe sourceはレビュー後に削除済みであり、このexact variantは現時点でrepository内のdurable fixtureではない。tracked 50054の既存gzip fixture（flag `0`だがgrpc-status欠落）でもext-grpcはpayloadをdeserializeし、最終statusだけをmissing-status由来の`UNKNOWN / Stream removed`にしたため、headerだけではpayloadを捨てないという核心は独立に観測できる。
- Reliability judgment: gzip flag `0`の結果はprotocol定義とofficial implementation sourceにも一致し、compiler profileやartifact packagingで変わる性質ではないため、GitHubのcorrectness reviewに用いてよい。ただし表現は「official PECL ext-grpc **1.80.0** on PHP 8.4.20/aarch64で観測」と限定し、canonical artifactを使った、または全ext-grpc versionで同一とは書かない。future rebuildではunversioned Dockerfileが別versionを取得し得るため、再現時はruntime `phpversion('grpc')`を必ず記録し、可能ならcanonical tag `1.80.0-php8.4-trixie-arm64-pecl`で再実行する。
- Missing-status judgment: tracked `no-trailers` fixtureに対するext-grpc `1.80.0`のunary `BenchReply / UNKNOWN(2) / Stream removed`、server streaming `1 message / UNKNOWN(2) / Stream removed`は再現可能なempirical observationとして十分である。一方、broken-responseのsynthesized code/detailsはversion・implementation policy差があり得るため、これは「INTERNALが誤り」というnormative証拠ではなく、grpc-go strictnessとext-grpc 1.80.0 drop-in behaviorが分かれるDesign Decisionの証拠としてだけ使う。
- Target-version caveat: `composer.lock`のwrapperは1.80.0だが、`composer.json`のdependency-resolution platformは`ext-grpc 1.62.0`、既存diagnostic image workflowのdefault comparatorは1.58.0であり、repository全体で唯一のofficial extension baselineが1.80.0へ固定されているわけではない。1.58/1.62互換まで主張または要求する場合は各versionのartifactで別途確認する。一つのcurrent official versionがPRのgenericな「official ext-grpc alignment」と異なるという反証には1.80.0観測で足りる。
