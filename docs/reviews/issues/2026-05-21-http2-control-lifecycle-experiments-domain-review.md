# HTTP/2 control lifecycle experiments domain review 2026-05-21

## Scope

- `docs/issues/open/2026-05-21-http2-control-lifecycle-experiments.md`
- `ext/grpc/transport.c`
- `ext/grpc/main.c`
- `ext/grpc/internal.h`
- `ext/grpc/tests/002-ini.phpt`

## Reviewer Role

- Domain model reviewer

## Review Prompt Summary

- Staged HTTP/2 control lifecycle experiment changesを、repository固有のHTTP/2/gRPC domain model、命名、責務境界、lifecycle/state transition、不変条件、error taxonomy、public/internal boundary、production vs diagnostic/test boundaryの観点で確認した。
- この変更はexperimental/default-off diagnostic featureとして扱い、production defaultではない前提でseverityを分類した。

## Issues

### REVIEW-20260521-001: response WINDOW_UPDATE experiment returns credit before response consumption is modeled

- Severity: `Low`
- Status: `Closed`
- Reviewer role: `Domain model reviewer`
- Finding: `grpc_lite.http2_experimental_response_window_update` は response `DATA` chunk受信直後、gRPC message validation/queueing前にconnection-level `WINDOW_UPDATE` を `len` 分submitしている。名前とissue本文は「response後」のcontrol lifecycle実験だが、実装上のdomain eventは「inbound DATA observed」であり、「response message consumed」または「application/server-streaming queueへ安全に移譲済み」ではない。
- Evidence: `ext/grpc/transport.c:create_h2_connection`; `ext/grpc/transport.c:on_data_chunk_recv_callback` lines 2026-2040; `ext/grpc/transport.c:grpc_protocol_process_response_data_direct`; `ext/grpc/transport.c:server_streaming_read_ahead_limit_would_exceed`
- Expected model: HTTP/2 flow-control creditはconnection/streamの受信capacityを表す。repository modelではserver streamingのread-ahead queue/backpressure、message size/compression/malformed frame handling、RST_STREAM lifecycleがgRPC Call側のsemantic acceptanceを決めるため、diagnosticでも「observed bytes」と「consumed/accepted response bytes」は明示的に区別されるべき。
- Why it matters: default-off実験としては許容可能だが、結果を将来の採用候補へ昇格すると、server streamingのslow consumer/backpressureやinvalid response pathでもconnection creditを先に返す設計として誤読される。SELECT 1観測では問題が出にくく、domain boundaryの違いがtrace結果の解釈に混ざる。
- Recommended fix: 現実装を維持するならINI名またはdocsを `http2_experimental_data_chunk_window_update` / `DATA受信時connection WINDOW_UPDATE` のように「observed DATA chunk」実験として明記する。本実装候補へ進める場合は、gRPC message validation/queue acceptance後にcreditを返す設計、またはconnection-level onlyである理由とserver streaming read-aheadとの関係を別issueで定義する。
- Fix summary: INI名を `grpc_lite.http2_experimental_data_chunk_window_update` に変更し、issue本文に「response消費」ではなく「DATA chunk観測」時点のconnection-level `WINDOW_UPDATE` 実験であることを明記した。
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh`; `./tools/test/check-c-static-analysis.sh`
- Notes: production defaultではないためLow。HTTP/2 control frame lifecycleの測定実験としては有効だが、flow-control modelとしては「消費」ではなく「観測」のcredit returnである。

### REVIEW-20260521-002: experimental INI flags are modeled as unconstrained longs instead of booleans

- Severity: `Low`
- Status: `Closed`
- Reviewer role: `Domain model reviewer`
- Finding: 2つのexperimental toggleが `zend_long` + `OnUpdateLong` として公開され、truthy longとして扱われている。既存のdiagnostic/control flagsである active BDP系は boolean INIとして登録されており、この変更のdomain conceptも「profile/toggleのon/off」であってnumeric parameterではない。
- Evidence: `ext/grpc/internal.h` lines 623-626; `ext/grpc/main.c` lines 13-16 and 47-50; `ext/grpc/transport.c:create_h2_connection` line 1684; `ext/grpc/transport.c:on_data_chunk_recv_callback` line 2035; `ext/grpc/tests/002-ini.phpt` lines 17-18
- Expected model: public operational surfaceに出るdiagnostic feature flagは、enabled/disabledの状態空間だけを持つ。numeric tuning parameterとboolean lifecycle toggleはINI型・global型・test名で区別されるべき。
- Why it matters: `2` や `-1` などの値が有効扱いになり、trace条件の再現性やreview時の状態空間が不要に広がる。default-offなのでproduction riskは低いが、diagnostic resultの条件記録としては曖昧になる。
- Recommended fix: `STD_PHP_INI_BOOLEAN` + `OnUpdateBool` を使い、module globalsも `bool` にする。PHPTではdefault `0` に加えて必要ならPHP_INI_SYSTEMでruntime変更不可であることだけを確認する。
- Fix summary: experimental toggleを `bool` module global + `STD_PHP_INI_BOOLEAN` / `OnUpdateBool` に変更し、numeric tuning parameterではなくon/off flagとして表現した。
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh`; `./tools/test/check-c-static-analysis.sh`
- Notes: experimental/default-offなのでLow。ただし次の実験flag追加前に揃えるとdiagnostic surfaceが読みやすくなる。

### REVIEW-20260521-003: “official settings” name overstates an observed ext-grpc profile as an official protocol model

- Severity: `Design Decision`
- Status: `Closed`
- Reviewer role: `Domain model reviewer`
- Finding: `http2_experimental_official_settings` と `GRPC_LITE_OFFICIAL_HTTP2_WINDOW_SIZE` は、HTTP/2/gRPC仕様上のofficial設定ではなく、issue本文にある「official ext-grpc 1.58.0 traceで観測したprofile」を表している。domain nameが「official protocol semantics」と「observed comparator profile」を混同している。
- Evidence: `docs/issues/open/2026-05-21-http2-control-lifecycle-experiments.md` progress section; `ext/grpc/main.c` lines 15 and 187; `ext/grpc/transport.c` lines 19-22 and 1684-1694; `ext/grpc/tests/002-ini.phpt` line 17
- Expected model: repositoryではext-grpc比較値は目標値ではなく観測線であり、production behaviorとdiagnostic/test/benchmark boundaryを分ける。HTTP/2 settings profileも「spec official」ではなく「observed ext-grpc/Core 1.58 comparator profile」として命名・記録されるべき。
- Why it matters: 将来このflagやconstantsを読んだ実装者が、`MAX_FRAME_SIZE=4194304` やunknown setting `65027` を標準的・望ましいgRPC設定と誤解する可能性がある。今回のdefault-off実験としては受け入れ可能だが、採用候補へ進める前に命名の意図を明確にする必要がある。
- Recommended fix: このまま実験issue内で閉じるなら、判断ログに「officialはcomparator image由来の観測profile名であり、HTTP/2/gRPC official semanticsではない」と明記してAcceptedにする。コードを残すなら `http2_experimental_ext_grpc_158_settings_profile` や `GRPC_LITE_EXT_GRPC_158_HTTP2_WINDOW_SIZE` のような観測元を含む名前へ変更する。
- Fix summary: INI名と定数名を `official` から `ext_grpc_158` に変更し、issue本文に「HTTP/2/gRPC仕様上のofficial semanticsではなく比較対象imageで観測したprofile」と明記した。
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh`; `./tools/test/check-c-static-analysis.sh`
- Notes: 意図的な短期実験名ならDesign Decisionとして扱える。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `0`
- Design Decision: `0`

## Re-review 2026-05-21

- Scope: previous Low findings and Design Decision only, plus updated naming/docs for newly introduced Blocker/High/Medium/Low issues.
- Result: previous findings remain resolved. `data_chunk_window_update` now models the observed DATA chunk event explicitly, experimental INI flags are boolean toggles, and the ext-grpc settings profile naming no longer presents the comparator trace profile as HTTP/2/gRPC official semantics.
- New issues from updated naming/docs: none.

## Re-review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Re-review 2026-05-21 initial SETTINGS handshake wait

- Scope: `grpc_lite.http2_experimental_wait_initial_settings_ack`, `h2_connection` SETTINGS handshake state, `wait_initial_settings_handshake()`, setup deadline/error propagation, and issue documentation for the new experiment.
- Result: no new domain model findings. The new INI is a default-off boolean diagnostic toggle, the SETTINGS handshake markers are owned by `h2_connection`, and the wait is modeled as connection setup lifecycle before first stream creation rather than as gRPC Call or PHP surface state.
- SETTINGS ACK lifecycle: acceptable for this experiment. `peer_settings_received` tracks inbound peer `SETTINGS`; `client_settings_ack_received` tracks peer ACK of the client's initial `SETTINGS`; `wait_initial_settings_handshake()` drains inbound frames and flushes pending nghttp2 control frames before allowing the first RPC stream.
- Deadline/error handling: acceptable for this experiment. The wait reuses the connection setup deadline, marks setup failures on the connection, and maps timeout to `HTTP/2 transport deadline exceeded` during connection creation.
- Public/internal/diagnostic boundary: acceptable. The flag is `PHP_INI_SYSTEM`, boolean, default off, and documented as an experiment rather than production default behavior.
- New issues from updated naming/docs: none.

## Re-review Result: initial SETTINGS handshake wait

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
