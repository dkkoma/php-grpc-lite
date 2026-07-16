# 1xx informational response adversarial consolidated pass 11 2026-07-15

## Scope

- 対象 commit: `20c2dc0`, `6a4902f`, `0e22a8a`, `a80556f`, `6168e2e`, `bf1f324`
- 対象 branch / HEAD: `codex/issue-informational-1xx-response-handling` / `bf1f324`
- 仕様 issue: `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- 過去 record: pass 1 / pass 3 の protocol, C safety, test review、pass 2 / pass 4 domain-model gate、pass 5 / pass 7 / pass 9 adversarial review、pass 6 / pass 8 / pass 10 domain-model gate
- 主対象: END_HEADERS 未完了 status block の close-after-pending-flush、connection terminal quarantine と sibling lifecycle、block-local Trailers-Only candidate、PHPT 042 / 043 と C unit の識別力

## Reviewer Role

- consolidated adversary: protocol + C safety / lifetime + test / fixture

## Review Prompt Summary

- pass 11 の convergence check として、pass 3 以降の修正が stated failure mode を閉じているか、特に最新 increment `bf1f324` が deadline なし call、multiplex sibling、CANCEL の socket flush、Trailers-Only metadata ownership を正しく扱うかを adversarial に確認した。
- shared wire-budget owner の overflow / reset、`TEMPORAL_CALLBACK_FAILURE` と `RESOURCE_EXHAUSTED` の precedence、pushed-stream attribution、late frame の owner lookup、production / diagnostic の phase transition も静的に照合した。
- repository policy と review hard rule に従い test suite / Docker は実行せず、code、diff、history、docs、fixture、既存 test oracle のみを確認した。

## Issues

### REVIEW-20260715-001: terminal quarantine の generic flush は sibling DATA で deadline なしに再停止できる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `protocol + C safety / lifetime adversary`
- Finding: END_HEADERS 未完了の nonterminal status block を検出すると target の `RST_STREAM(CANCEL)` と connection terminal quarantine は queue されるが、その直後に呼ぶ flush は target RST 専用ではなく、nghttp2 session にある全 pending frame を送る。別 stream に flow-control deferred の大きな request DATA があり、peer がその window を開いてから読み取りを止めると、flush は sibling DATA まで駆動して socket backpressure で停止する。target call に deadline がなければ write deadline も 0 のため `poll(POLLOUT, -1)` となり、connection を dead にする地点へ到達しない。
- Evidence:
  - `src/transport.c:2292-2327` の `grpc_protocol_observe_response_status_field()` は target RST を submit し、END_HEADERS がない場合に `close_after_pending_flush` を設定する。`src/unary_call.c:246-253` と `src/server_streaming_call.c:324-333` はその後 `send_pending_h2_frames(connection, call)` を呼ぶ。
  - `src/transport.c:2008-2080` の `send_pending_h2_frames_with_deadline()` は `nghttp2_session_send()` を一度完走させ、coalesced buffer 全体を socket へ flush してから `mark_connection_dead()` する。target RST までで送信を打ち切る gate はなく、同じ session の sendable sibling DATA も対象になる。
  - 同関数は `call->deadline_abs_us` を write deadline に使う。deadline なし call では 0 のままであり、`src/transport.c:1285-1289,1326-1348` により EAGAIN 後の write poll は無期限になる。対照的に通常の `cancel_grpc_call_stream()` は、全 pending frame を flush する性質を明記した上で `GRPC_LITE_CANCEL_RST_WRITE_GRACE_US` の固定 grace を使う（`src/transport.c:332-375`）。
  - 現在の `multiplex-hold-sibling` fixture は sibling の小さい request を受信し終えた後に1 messageを返すため、client側に deferred outbound DATAを残さない。またfixtureはconnectionを読み続けるためsocket backpressureも作らず、PHPT 042の「target `rpc.end` 後に追加I/Oなし」というoracleはこのpre-terminal stallを識別しない。
  - exact runtime probe: fixtureが `SETTINGS_INITIAL_WINDOW_SIZE=1024` をadvertiseし、clientは大きなrequest payloadを持つserver-streaming sibling Aを開始する。fixtureはAのrequest HEADERSを見た時点でinitial responseと1 messageを返し、Aの残りrequest DATAをflow-control deferredに保つ。別stream Bへ103とEND_HEADERSなしのfinal initial status blockを送る直前にAとconnectionのWINDOW_UPDATEを送り、その後peerはsocketを読まない。Bはdeadlineなしとする。期待oracleは固定上限内にBが`UNKNOWN`、Aが`UNAVAILABLE`へ収束し、quarantine開始後にAの追加DATAを送らないことである。現状はgeneric flushがA DATAを送り始め、送信bufferを満たすと無期限に停止できる。
- Expected model: incomplete inbound HPACK block の terminal quarantine は target CANCEL の best-effort flush を有限時間で終え、その後connectionを全ownerに対してterminalにする必要がある。quarantine開始後にsiblingのapplication DATAを新たにdriveしてはならず、peerのread progressがなくてもtargetとsiblingの終了がboundedでなければならない。
- Why it matters: adversarial peerは一つのmalformed responseと同一connection上の大きなsibling requestを組み合わせ、pass 9で対象だったdeadlineなしPHP workerのstallを再現できる。またterminalと判断したconnection上で無関係なsibling request bytesを追加送信するため、connection failureのblast radiusも拡大する。
- Recommended fix: close-after-pending-flushには通常のsession-wide send helperを使わず、固定graceを持つterminal-quarantine専用flushを設ける。target RSTが生成・socket flushされた地点までに限定し、それ以降のsibling DATAをdriveせずconnectionをdead化する。target RSTだけをnghttp2の通常送信順序から安全に分離できない場合でも、少なくともcall deadlineとは独立した短いwrite graceでstallをboundedにし、上記small-window / large-sibling / peer-read-stall probeを追加する。
- Fix summary: `send_pending_h2_frames_with_deadline()` の共通処理をdeadline指定の内部helperへ分離し、terminal quarantine専用の `flush_terminal_quarantine()` を追加した。quarantine flushはcall deadlineに依存しない固定50ms graceを使い、grace超過を対象callの`DEADLINE_EXCEEDED`へ再分類しない。`close_after_pending_flush` 後はDATA providerを`NGHTTP2_ERR_DEFERRED`としてsibling application DATAを新たにdriveせず、unary / server streamingは通常sendより先に専用flushを実行してconnectionをdead化する。raw fixtureは1024-byte stream window、16MiB sibling、target直前のconnection / sibling WINDOW_UPDATE、750ms peer read stallを組み合わせた。probe setupではtarget request用のconnection windowだけを補充し、sibling stream windowは枯渇させたままにすることで、quarantine開始前の循環待ちを避けつつdiscriminatorを維持した。
- Fix commit: `pending`
- Verification: `test-serverをrebuild / force-recreate後、./tools/test/check-phpt.sh PASS（29/29）。PHPT 042のmultiplex probeでdeadlineなしtargetが500ms上限内にUNKNOWN + invalid grpc-status trailerへ収束し、既存siblingがUNAVAILABLE + incomplete HTTP/2 response header blockへ収束すること、peerがtarget RST_STREAM(CANCEL)を受信すること、quarantine開始後のsibling DATAがないこと、follow-upがfresh connectionで成功することを確認した。./tools/test/check-c-unit.sh PASS（4/4）、PHPUnit PASS（31 tests / 116 assertions）、./tools/test/check-c-static-analysis.sh PASS（findings none）。`
- Notes: pass 9 `REVIEW-20260715-001` の修正が、pending sibling DATA と socket backpressure の組合せではstated liveness failureを閉じ切らないことを示す新規 finding。accepted Decision Log の再検討ではない。

### REVIEW-20260715-002: PHPT 042 の exact CANCEL oracle は socket flush 前の trace だけで成立する

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `test / fixture adversary`
- Finding: PHPT 042はEND_HEADERS未完了blockごとにexact `RST_STREAM(CANCEL)` が送られたと主張するが、oracleはclient traceの `wire.frame_out` だけを数える。このeventはnghttp2のsend callbackがframe bytesをcoalesced bufferへ追加した直後、実socket flushより前に記録されるため、peerへ1 byteも届かない実装でもtestが通る。
- Evidence:
  - `src/transport.c:1984-2005` の `send_callback()` は `h2_connection_buffer_or_write()` の後に `grpc_lite_trace_outbound_frame()` を呼ぶ。write coalescing中はbytesが `connection->write_buffer` に残り、実socket送信は `src/transport.c:2031-2051` の `nghttp2_session_send()` 後の `h2_connection_flush_write_buffer()` で初めて行われる。
  - 同ファイルのtest seam `terminal-status-rst-flush-fatal` は、`wire.frame_out` が生成済みでもpre-socket flushでbytesを破棄できることを具体的に示している（`src/transport.c:2039-2049`）。したがってtrace eventはwire到達の証拠ではない。
  - `tests/phpt/042-informational-1xx-adversarial.phpt:174-190,214-247` はtrace上のframe count、stream ID、error codeだけをassertする。`poc/test-server/main.go:648-718` がpeer受信RSTを記録するのはresource / silent-status probeだけで、incomplete block controlsは追跡対象にしない。`valid-after-incomplete-status` と `valid-after-terminal-quarantine` は専用caseを持たずdefault OKとなるため、fresh follow-upもprior RSTのpeer受信を証明しない。
  - discriminator mutation: `nghttp2_session_send()` がtarget RSTをcoalesced bufferへ生成した後、socket flushを省略して成功扱いのままconnectionをdead化する。現行PHPT 042はtarget `UNKNOWN`、trace上のexact CANCEL、sibling `UNAVAILABLE`、fresh follow-up、新規prefaceの全assertを満たせるが、fixtureはRSTを受信しない。
- Expected model: testが「target CANCELをflushしてからconnectionをterminal化する」というwire actionを保証するなら、oracleはclient内部のframe生成ではなくpeerが対象streamの`RST_STREAM(CANCEL)`を受信したことを確認しなければならない。traceはattributionの補助には使えるがsocket deliveryの代用にはならない。
- Why it matters: close-after-pending-flush実装が将来、RST生成後にbufferを捨てる、flush順序を誤る、成功を早く返す回帰を起こしても、現在の中心的なconvergence testが検出できない。docsの「clientの即時CANCEL、flush後のconnection terminal化」「exact CANCEL」というcovered表記ともoracleが一致しない。
- Recommended fix: fixtureのconnection外にprobe stateを保持し、authorityまたは一意tokenとtarget streamを対応付けてpeer受信 `RST_STREAM(CANCEL)` を記録する。fresh follow-up controlはそのmarkerがある場合だけOKを返すようにする。multiplex controlもtarget streamのpeer受信RSTを同様にgateし、trace assertionはstream attributionと「余分なRSTなし」の確認に限定する。
- Fix summary: test-server process内にauthority単位の `informationalCancelProbeStore` を追加し、各fixture connectionではincomplete blockのtarget stream IDとprobeを対応付けた。peerが対象streamの`RST_STREAM(CANCEL)`を実受信した場合だけmarkerを成立させ、fresh follow-upの `require-prior-incomplete-status-cancel` はmarkerをconsumeできた場合だけOKを返す。multiplex probeは同じmarkerに加えてtrigger後のsibling DATA不在を要求する。PHPT 042の6個のunary / server streaming incomplete controlsとmultiplex controlはこのpeer-side gateを通し、client traceはtarget attribution、exact count、error codeの補助oracleに限定した。
- Fix commit: `pending`
- Verification: `test-serverをrebuild / force-recreate後、./tools/test/check-phpt.sh PASS（29/29）。PHPT 042で3種類のEND_HEADERS未完了status fieldについてunary / server streamingの各target、ならびにmultiplex targetのpeer-side CANCEL markerをfresh follow-upがconsumeして成功することを確認した。multiplexではpeer受信CANCELとtrigger後のsibling DATA不在を同じgateで確認した。./tools/test/check-c-unit.sh PASS（4/4）、PHPUnit PASS（31 tests / 116 assertions）、./tools/test/check-c-static-analysis.sh PASS（findings none）。`
- Notes: REVIEW-20260715-001 のproduction defectとは独立したoracle finding。bounded flushへ修正した後もpeer到達を識別するために必要である。

### REVIEW-20260715-003: PHPT 043 の2 iteration control は wire byte counter の reset を識別しない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `test / fixture adversary`
- Finding: `valid-informational-iteration-reset` は2 iterations累積で128-entry上限を越えるため `wire_response_header_entry_count` のresetを識別するが、各iterationのdecoded name/value bytesは約786 bytesにすぎず、2回累積してもdefault 64KiB byte上限から遠い。`wire_response_header_bytes = 0` のresetだけを削除してもPHPT 043は成功する。
- Evidence:
  - `src/diagnostic/bench.c:1504-1520` は各iterationでphase、entry counter、byte counterをresetするが、`tests/phpt/043-informational-1xx-bench-parity.phpt:31-33` は `valid-informational-iteration-reset` の2 iterationsがともOKであることだけをassertする。
  - `poc/test-server/main.go:909-920` の1 iterationはstatus-only 103を60個、6 fieldsのpollution block、3 fieldsのvalid gRPC responseで合計69 entriesとなる。一方、budgetが数えるdecoded `name length + value length` は約786 bytes/iterationであり、byte counterが累積しても2 iterationsで約1.6KiBにしかならない。
  - `docs/verification/test-fixtures.md:111` はこのcontrolがsemantic stateと「wire counter」のiteration resetを同時に検証すると記載し、`docs/verification/verification-matrix.md:30` もdiagnosticのdefault 64KiB rejectionとiteration resetをcoveredとしてまとめている。しかしentry resetとbyte resetは別fieldであり、現在のoracleは前者しか識別しない。
  - discriminator mutation: `src/diagnostic/bench.c:1520` のbyte resetだけを除去する。entry counterは各iterationでresetされるため各回69 entriesで通り、累積bytesも64KiB未満なので現行 `ok=2 / failed=0` は変わらない。
- Expected model: reusable diagnostic call stateの各wire-budget dimensionはiteration境界で独立にfreshでなければならず、reset testはentry countとdecoded byte countのどちらか一方がstaleでも失敗する必要がある。
- Why it matters: production / diagnostic parityのために追加したshared wire-budget ownerで、byte counterのreset regressionだけが無検出になる。低いmetadata byte limitや繰り返しbatchでは、前iterationのinformational headersによって正常な後続iterationが誤って`RESOURCE_EXHAUSTED`になる。
- Recommended fix: byte reset専用のvalid controlを追加する。例えば各iterationに約8KiBの`x-info`を持つinformational blockを4個とvalid gRPC OKを返せば、entry数は十分低いまま1 iterationは64KiB未満、2 iterations累積は64KiB超となる。そのcontrolを2 iterationsで実行し、両方OKをassertする。既存69-entry controlはentry reset専用として残す。
- Fix summary: 8192-byteの `x-info` を4個の103 blockで返す `valid-informational-byte-iteration-reset` controlを追加した。1 iterationは約32.8KiB / 11 entriesで各上限内、byteだけを2 iterations累積するとdefault 64KiBを超え、entry累積は128未満に留まる。PHPT 043は既存の69-entry controlをentry-counter reset専用として残し、新controlを2 iterations実行してwire byte counter resetを独立に固定した。fixture / verification docsもentry / byte resetを分けて記載した。
- Fix commit: `pending`
- Verification: `test-serverをrebuild / force-recreate後、./tools/test/check-phpt.sh PASS（29/29）。PHPT 043でentry-reset controlとbyte-reset controlをそれぞれ2 iterations実行し、双方がok=2 / failed=0となることを確認した。./tools/test/check-c-unit.sh PASS（4/4）、PHPUnit PASS（31 tests / 116 assertions）、./tools/test/check-c-static-analysis.sh PASS（findings none）。`
- Notes: pass 3 test `REVIEW-20260715-003` で追加されたbench iteration reset probeの識別範囲がentry counterに限られていることを示す新規 finding。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
