---
Status: Open
Owner: Codex
Created: 2026-06-07
Branch: codex/hotpath-early-return-trial
---

# hotpath early return / slow path splitでinline costを下げられるか確認する

## 目的

Clang optimization remarksでinline cost / thresholdが見えるようになったため、C拡張のhot pathで early return と rare/error path の分離が、可読性とoptimizer判断の両方に効くかを小さく検証する。

性能改善の採用ではなく、まず「どの関数で cost が下がるか」「DSO上の `.text` / symbol shape がどう変わるか」「controlled benchで悪化しないか」を確認する。

## 背景

`docs/issues/closed/2026-06-06-release-artifact-lto-dso-investigation.md` で、Clang ThinLTOのoptimization remarksから以下のような判断情報を読めることを確認した。

- `h2_connection_send` は caller へは cost 1560 / threshold 225 でinlineされない。
- `configure_tls_connection` は `create_h2_connection` へ cost 2555 / threshold 225 でinlineされない。
- 小さいhelperは cost model によりinlineされる一方、I/O、trace、TLS、error detail処理を含む関数はtoo costlyになりやすい。

この結果から、単純に `always_inline` を付けるのではなく、まずhot pathの読みやすさを損なわない範囲で early return と slow path split を試す。

## 仮説

- hot path内に混在しているrare/error pathを early return または cold helperへ分けると、正常系の直線的な範囲が読みやすくなる。
- Clang remarks上で対象callsiteの cost が下がる、または missed inline の理由が変わる可能性がある。
- ただしI/Oやnghttp2 callbackが支配的な関数では、`.text` やbenchに見える差は小さい可能性が高い。

## スコープ

- Clang optimization remarksから、hot path候補の missed inline と cost / threshold を確認する。
- `transport.c` 周辺の小さい候補に限定し、early return / slow path split を1〜2箇所だけ試す。
- before/afterで以下を比較する。
  - Clang optimization remarks
  - Bloaty / `size -A` / symbol diff
  - controlled CPU micro bench
  - PHPT / C unit / static analysis

## 非スコープ

- `always_inline` の大量付与。
- `-O3` / LTO / PGO のdefault採用。
- transport lifecycleやHTTP/2/gRPC semanticsの変更。
- 大規模な `transport.c` 分割や責務変更。
- ext-grpcの数値に近づけること自体を目的にすること。

## 計画

1. 現在の `main` 起点 + LTO DSO調査issue取り込み状態で、Clang remarksの対象候補を再確認する。
2. 候補を1〜2箇所に絞り、beforeのremarks / DSO / controlled benchを記録する。
3. early returnまたはslow path splitを小さく実装する。
4. afterのremarks / DSO / controlled benchを同条件で取り、改善・悪化・ノイズを分類する。
5. 効果が小さい、または複雑性が増える場合は採用せず、調査結果として閉じる。

## 採否基準

- 採用候補:
  - hot pathの可読性が上がる。
  - PHPT / C unit / static analysis が通る。
  - controlled benchで悪化しない。
  - remarksまたはDSO上で、対象関数のcost低下、symbol整理、`.text`縮小などの説明可能な差分がある。
- 棄却候補:
  - cost / DSO / benchに意味のある差がない。
  - cold helper化で責務やlifecycleが読みにくくなる。
  - error handlingやconnection lifecycleの見通しが悪くなる。

## 進捗

- 2026-06-07: issue作成。
- 2026-06-07: 作業ブランチを `main` 起点で作成し、前回のLTO DSO調査issueを取り込んだ。
- 2026-06-07: Clang/Bloaty診断を毎回手作業で組まなくてよいように、`dev-optimizer` compose service と `tools/diagnostic/optimizer-dso-report.sh` を追加した。
- 2026-06-07: request metadata header append pathで、capacity確認済みのappendを `append_request_header_unchecked()` として分離し、custom metadata pathではgrow済み確認後にunchecked appendを呼ぶ形を試した。
- 2026-06-07: 同条件before/afterでは実測改善を性能根拠として主張できないが、prechecked appendの責務が明確になり、局所remarksも改善したため、見通し改善としてtrial実装を採用する方針にした。
- 2026-06-07: 次候補として `on_data_chunk_recv_callback` のearly return化を試す。目的は、対象外stream / empty chunk / direct response decode / buffered response append の分岐を直線化し、response data callbackの責務を読みやすくすること。性能値ではなく、semantics維持と見通し改善を採否の主軸にする。
- 2026-06-07: 次候補として `preflight_persistent_connection` のTLS / plain socket probe分岐を小さく分離する。`drain_pending_connection_data_for_reuse` はHTTP/2 control frame drain / pending write flush / drain limit処理を含むため今回は触らず、idle connection reuse前のprobe判定だけを名前付きhelperへ出して見通しを確認する。
- 2026-06-07: 次候補として `on_header_callback` のheader分類を小さく整理する。gRPC status / message / content-type / encodingの副作用と、metadata listへの保存が混在しているため、trailing判定、special header side effect、metadata保存の境界を確認する。
- 2026-06-07: 次候補として `append_grpc_timeout_request_header` をcustom metadataと同じprechecked append形へ寄せる。`grpc-timeout` valueはowned `zend_string` として `value_strings` に登録する必要があるため、lifetime順序を変えずに最後のheader appendだけをunchecked化できるか確認する。

## 観測

### 同条件 before/after: Clang/Bloaty

`df5bce2` をbefore、`f22189e` をafterとして、同じ `dev-optimizer` 環境でDSO reportを取り直した。

| item | before | after | 判断 |
| --- | ---: | ---: | --- |
| commit | `df5bce2` | `f22189e` | 同じ診断環境 |
| clang | 19.1.7 | 19.1.7 | 同条件 |
| LLD | 19.1.7 | 19.1.7 | 同条件 |
| Bloaty | 1.1 | 1.1 | 同条件 |
| grpc.so bytes | 528,752 | 531,056 | debug情報込みで +2,304 bytes |
| `.text` | 53.6 KiB | 53.6 KiB | 実質不変 |
| clang inline success | 941 | 926 | 全体数では改善なし |
| clang inline missed | 171 | 167 | missed数は少し減ったが全体判断には弱い |

対象callsite:

| callsite | before | after | 判断 |
| --- | --- | --- | --- |
| `append_request_header` -> `append_custom_request_header_value` | not inlined, cost 65 / threshold 45 | `append_request_header_unchecked` が inlined, cost -5 / threshold 45 | 局所splitは成功 |
| `append_custom_request_header_value` -> `append_custom_request_headers` | not inlined, cost 1105 / threshold 225 | not inlined, cost 1025 / threshold 225 | costは下がったがthresholdには遠い |
| `is_binary_metadata_header` in `append_custom_request_header_value` | 2回評価 | 1回評価 | 局所的な重複評価は解消 |

Bloaty symbols上位はbefore/afterとも `grpc_lite_unary_call_perform_on_connection`, `send_callback`, `create_h2_connection`, `server_streaming_call_open_resource`, `zim_Call_startBatch` で、今回の小変更がDSO上の大きなsymbol shape変更を起こしている様子はない。

### 同条件 before/after: bench

通常dev buildへ戻した後、before/afterでmetadata系benchを取った。baseline worktreeは `COMPOSE_PROJECT_NAME=php-grpc-lite` で既存compose project/networkを共有し、`vendor/` は同じ依存を一時コピーした。

`metadata-header --calls=200`:

| measurement | before p50 us | after p50 us | before p99 us | after p99 us | 判断 |
| --- | ---: | ---: | ---: | ---: | --- |
| req_0_resp_0_value_0b | 35.5 | 35.9 | 219.4 | 1116.7 | metadataなし、対象外ノイズ |
| req_10_resp_0_value_32b | 35.5 | 31.9 | 521.0 | 608.7 | p50は改善方向、p99は悪化方向 |
| req_10_resp_10_value_32b | 48.1 | 46.9 | 649.7 | 398.1 | 改善方向だがlatency系なので揺れあり |
| req_50_resp_0_value_32b | 101.9 | 103.6 | 1399.8 | 1185.8 | ほぼ不変 |
| req_50_resp_50_value_32b | 151.7 | 153.2 | 1565.2 | 696.7 | p50はほぼ不変、p99は改善方向 |

`cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3` の metadata case:

| measurement | before cpu_us/call | after cpu_us/call | 判断 |
| --- | ---: | ---: | --- |
| metadata_unary_req10_resp10_32b repeat 1 | 14.6 | 14.6 | 同等 |
| metadata_unary_req10_resp10_32b repeat 2 | 14.6 | 14.8 | 微悪化 |
| metadata_unary_req10_resp10_32b repeat 3 | 14.3 | 15.3 | 悪化方向 |
| average | 14.5 | 14.9 | 改善なし、ノイズ込みで微悪化寄り |

bench上は、metadata-heavy条件でも性能改善として主張できる差は見えない。`metadata-header` のp99は揺れており、CPU summaryを優先すると「局所remarksは改善したが、実測性能改善ではない」と見るのが妥当。一方で、custom metadata pathでは直前にcapacity確認済みであることがコード上に現れ、checked append / unchecked appendの責務分離としては見通しがよくなるため、採用理由は性能ではなく構造の明確化とする。

### 候補2: response data callback

`on_data_chunk_recv_callback` は、nghttp2 DATA chunk callbackとして以下を同じ入れ子の中で扱っている。

- 対象callがないstreamを無視する。
- `stream_id` がcallと一致しないchunkを無視する。
- empty chunkを無視する。
- direct response modeでは incremental decode pathへ渡す。
- buffered response modeではmessage length validation後、必要ならbodyへappendする。

この候補では、対象外条件をearly returnし、direct pathとbuffered pathを分ける。`discard_response_body` は従来通りlength validation後に評価する。これは、不正なmessage lengthはbody discard中でも検出する現在の挙動を保つため。

実装後:

- `call == NULL` は従来通り無視して `0` を返す。
- `stream_id` mismatch と `len == 0` はearly returnする。従来の外側条件不一致時と同じ挙動。
- direct response pathは `grpc_protocol_process_response_data_direct()` 成功後に即 `return 0` する。server streaming queue delivery chunkをunary body validation / appendへ落とさないため。
- buffered response pathは `grpc_protocol_validate_response_message_lengths()` 後に `discard_response_body` を評価する。従来通り、discard中でもmalformed frame / length violationは検出する。

`tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-data-callback-early-return`:

| item | before candidate2 | after candidate2 | 判断 |
| --- | ---: | ---: | --- |
| grpc.so bytes | 531,056 | 531,056 | 不変 |
| `.text` | 53.6 KiB | 53.6 KiB | 不変 |
| clang inline success | 926 | 924 | 全体差はノイズ相当 |
| clang inline missed | 167 | 169 | 全体差はノイズ相当 |

対象callsite:

- `grpc_protocol_process_response_data_direct` は引き続き `on_data_chunk_recv_callback` へinlineされない: cost 1370 / threshold 225。
- `grpc_protocol_validate_response_message_lengths` は引き続き `on_data_chunk_recv_callback` へinlineされない: cost 515 / threshold 225。
- DSO / remarks上の性能改善は見えない。採用理由は、nghttp2 DATA callbackの対象外条件、direct decode path、buffered append pathが直線的に読めること。

Subagent protocol check:

- early return for `call == NULL`, `stream_id` mismatch, `len == 0` は既存のno-op returnと同等。
- direct server-streaming pathは成功後に即returnする必要がある。今回の実装はこれを満たす。
- unary validationを `discard_response_body` より前に残す必要がある。今回の実装はこれを満たす。

### 候補3: persistent connection preflight

`preflight_persistent_connection` は、persistent cacheから取り出したHTTP/2 connectionを再利用してよいかを判断する入口で、次の責務を持つ。

- 既にdead / drainingなconnectionは再利用しない。
- active streamが残るconnectionはprobeせず利用可能として扱う。
- idle TLS connectionでは `SSL_peek()` でpending data / EOF / TLS errorを判定する。
- idle plain socket connectionでは `recv(MSG_PEEK | MSG_DONTWAIT)` でpending data / EOF / socket errorを判定する。
- pending dataがあれば `drain_pending_connection_data_for_reuse()` に委譲し、nghttp2 sessionへ既着bytesを流し、必要なpending control frameをflushする。

今回の候補では `drain_pending_connection_data_for_reuse()` を変更しない。過去レビューで修正した「pending control frame flush」「bounded drain limit」「WANT_READ/EAGAIN境界」を含むため、ここを触るとHTTP/2 lifecycle変更のリスクが大きい。代わりに、TLS probeとplain socket probeを小さいhelperへ分け、top-levelを `connection_usable` / active stream / transport種別dispatchだけにする。

保持すべきsemantics:

- `connection_usable(connection)` がfalseなら必ずfalseを返し、connection stateを変えない。
- `active_stream_count > 0` は従来通りtrueを返す。
- `SSL_peek() > 0` と `recv() > 0` は即reuse可ではなく、必ずdrain helperへ渡す。
- `SSL_ERROR_WANT_READ` / `SSL_ERROR_WANT_WRITE` と `EAGAIN` / `EWOULDBLOCK` はidle boundaryとしてtrueを返す。
- TLS close / socket EOF / hard errorは従来通りerror detailとdead stateを設定してfalseを返す。

実装後:

- `preflight_tls_connection_for_reuse()` と `preflight_socket_connection_for_reuse()` を追加し、TLS / plain socket固有のidle probeを分けた。
- `preflight_persistent_connection()` は `connection_usable`、`active_stream_count`、TLS-or-socket dispatchだけを扱う入口になった。
- `drain_pending_connection_data_for_reuse()` は変更していない。

`after-data-callback-early-return` をbefore、`after-preflight-probe-split` をafterとして、同じ `dev-optimizer` 環境で比較した。

| item | before candidate3 | after candidate3 | 判断 |
| --- | ---: | ---: | --- |
| grpc.so bytes | 531,056 | 532,096 | debug情報込みで +1,040 bytes |
| `.text` | 54,872 | 54,944 | +72 bytes |
| clang inline success | 924 | 932 | 全体数だけでは判断しない |
| clang inline missed | 169 | 175 | 全体数だけでは判断しない |

対象callsite:

- `preflight_tls_connection_for_reuse` と `preflight_socket_connection_for_reuse` は `preflight_persistent_connection` へinlineされた。
- `drain_pending_connection_data_for_reuse` は引き続きinlineされない。TLS側 cost 1755 / threshold 225、socket側 cost 1745 / threshold 225。
- `preflight_persistent_connection` -> `get_persistent_connection` は引き続きinlineされず、costは 760 / threshold 225 から 820 / threshold 225 へ増えた。

この候補はoptimizer上の改善ではない。採用する場合の根拠は、persistent connection reuse入口で「usable」「active stream」「TLS probe」「plain socket probe」のlifecycle条件が分かれて読めることに限定する。

Subagent protocol/lifecycle check:

- helper化自体は、`drain_pending_connection_data_for_reuse()` を触らない限りHTTP/2 lifecycle上のリスクは低い。
- `SSL_peek() > 0` / `recv(MSG_PEEK | MSG_DONTWAIT) > 0` をsafe reuseへ誤分類せず、必ずdrainへ渡すことが重要。
- TLS WANT系では `last_ssl_error` を更新せず、hard errorでのみ更新する現状を維持する。
- socket hard errorで追加diagnosticを入れるとbehavior changeになるため、今回のrefactorでは行わない。

### 候補4: response header callback

`on_header_callback` は、nghttp2 header callbackとして以下を1つの関数内で扱っている。

- stream idから対象callを引く。
- initial headersかtrailersかを判定する。
- `grpc-status` / `grpc-message` / `grpc-status-details-bin` をgRPC status metadataとして扱う。
- `:status` / `content-type` / `grpc-encoding` からHTTP/gRPC validation stateを更新する。
- 最終的に受信metadata entryとして保存する。

保持すべきsemantics:

- 対象callがないstreamとstream mismatchは従来通り無視する。
- `frame->headers.cat != NGHTTP2_HCAT_RESPONSE` はtrailingとして扱う。
- initial response headers内で `grpc-status` を見た場合は trailers-only response として既存metadataをtrailing扱いへ移す。
- initial response headers内で既に `grpc_status >= 0` の場合、後続headerはtrailing扱いにする。
- `grpc-message` は既存 `grpc_message` をreleaseしてdecode後の文字列へ置き換える。
- invalid `content-type` とunsupported `grpc-encoding` は `discard_response_body` を立てる。
- special headerも従来通りmetadata entryとして保存する。

実装後:

- `response_header_initially_trailing()` を追加し、initial headers / trailers / trailers-only後続headerの分類を切り出した。
- `apply_response_header_state()` を追加し、`grpc-status` / `grpc-message` / `grpc-status-details-bin` / `:status` / `content-type` / `grpc-encoding` の副作用をまとめた。
- `on_header_callback()` は stream guard、trailing初期判定、special header side effect、metadata entry保存を順に呼ぶ入口になった。

`after-preflight-probe-split` をbefore、`after-header-callback-state-split` をafterとして、同じ `dev-optimizer` 環境で比較した。

| item | before candidate4 | after candidate4 | 判断 |
| --- | ---: | ---: | --- |
| grpc.so bytes | 532,096 | 533,296 | debug情報込みで +1,200 bytes |
| `.text` | 54,944 | 54,944 | 不変 |
| clang inline success | 932 | 934 | 全体数だけでは判断しない |
| clang inline missed | 175 | 172 | 全体数だけでは判断しない |

対象callsite:

- `response_header_initially_trailing` は `on_header_callback` へinlineされた。
- `apply_response_header_state` は `on_header_callback` へinlineされた。
- `grpc_protocol_decode_message` は引き続きinlineされない。cost 405 / threshold 225。
- `grpc_protocol_add_response_metadata_entry` は引き続きinlineされない。cost 395 / threshold 225。

この候補も性能改善ではない。`.text` は不変で、重いdecode / metadata保存helperのinline可否も変わっていない。採用する場合の根拠は、nghttp2 header callback内で「trailing分類」「special header state更新」「metadata保存」の順序が明示されることに限定する。

Subagent protocol/semantics check:

- `trailing` 初期値は副作用適用前に計算する必要がある。今回の実装はこれを満たす。
- `call->grpc_status >= 0` を `grpc_status_seen` に置き換えないこと。今回の実装は元の条件を維持している。
- initial response内の `grpc-status` だけが既存metadataをtrailingへ移す。今回の実装は元の条件を維持している。
- initial response内の `grpc-message` / `grpc-status-details-bin` も `initial_grpc_status_seen = true` にする。今回の実装は元の副作用を維持している。
- special headerも従来通りmetadata entryに保存する。今回の実装は保存呼び出しを省いていない。

### 候補5: grpc-timeout header append

`append_grpc_timeout_request_header()` は、deadlineからwire metadata `grpc-timeout` を生成し、valueを `zend_string` として保持してからrequest headerへ追加する。通常の固定headerと違い、header valueのstorage lifetimeを `h2_request_headers.value_strings` が所有するため、単純にstack bufferを指すことはできない。

保持すべきsemantics:

- `grpc_lite_format_timeout_us()` が0を返した場合はheaderを追加しない。
- `zend_string_init()` したvalueは、headerへ追加する前に `headers->value_strings` へ登録し、`free_request_headers()` で解放されるようにする。
- `headers->value_strings` に登録できない場合はvalueをreleaseしてreturnする。
- `headers->len` がcapacityに達している場合は従来通りgrowを試みる。
- grow後もcapacityが足りない場合はheaderを追加しない。

実装後:

- `append_grpc_timeout_request_header()` は、valueを `headers->value_strings` に登録した後、`headers->len` のcapacityを確認し、必要なら `grow_request_headers()` を呼ぶ。
- capacity確認後、`append_request_header_unchecked()` で `grpc-timeout` を追加する。
- `value_str` の登録 / release / free責務は従来通り `headers->value_strings` と `free_request_headers()` に残した。

`after-header-callback-state-split` をbefore、`after-grpc-timeout-unchecked-append` をafterとして、同じ `dev-optimizer` 環境で比較した。

| item | before candidate5 | after candidate5 | 判断 |
| --- | ---: | ---: | --- |
| grpc.so bytes | 533,296 | 533,024 | debug情報込みで -272 bytes |
| `.text` | 54,944 | 54,948 | +4 bytes |
| clang inline success | 934 | 944 | 全体数だけでは判断しない |
| clang inline missed | 172 | 175 | 全体数だけでは判断しない |

対象callsite:

- beforeは `append_request_header` が `append_grpc_timeout_request_header` へinlineされていた。cost 65 / threshold 225。
- afterは `append_request_header_unchecked` が直接 `append_grpc_timeout_request_header` へinlineされた。cost -5 / threshold 487。
- `grow_request_headers` は引き続きinlineされない。cost 335 / threshold 225。

この候補は、custom metadata pathと同じくprechecked appendを明示する整理だったが、性能改善とは主張できない。`.text` は +4 bytesで実質不変。`grpc-timeout` value lifetimeを変えず、subagent reviewでも登録済みvalueは `free_request_headers()` 解放に任せる方針が妥当と確認した。

ただし、後続のstep benchで候補5後にmetadata-heavy caseの悪化方向が見えた。可読性差分も小さく、bench riskに見合わないため、最終的には採用せずコード変更を戻した。

### 候補3-5 follow-up: bench before/after

候補3-5について、当初はoptimizer reportと検証のみで、bench before/afterを取っていなかった。見通し改善として採用する場合でも、性能悪化がないことを確認するため、`a46ba3e` をbefore、`cc9ff9b` をafterとして同条件benchを追加で取った。

`cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3`:

| measurement | before avg cpu_us/call | after avg cpu_us/call | 判断 |
| --- | ---: | ---: | --- |
| tiny_unary_0b | 11.8 | 16.8 | after repeat 3に27.1の外れ値。判断に使いにくい |
| small_unary_100b | 11.0 | 10.6 | 同等 |
| new_client_unary_100b | 12.8 | 12.6 | 同等 |
| metadata_unary_req10_resp10_32b | 14.9 | 15.4 | 微悪化方向 |
| begin_txn_unary | 10.7 | 10.9 | 同等 |
| commit_txn_unary | 10.5 | 10.7 | 同等 |
| small_streaming_1x100b | 11.7 | 10.9 | 同等〜改善方向 |
| tiny_streaming_1x0b | 11.4 | 11.1 | 同等 |
| new_client_streaming_1x100b | 13.4 | 12.9 | 同等 |
| small_streaming_10x100b | 16.6 | 16.9 | 同等 |
| small_streaming_100x100b | 59.3 | 57.6 | 同等〜改善方向 |
| select_1row_10col_streaming | 11.5 | 11.2 | 同等 |
| dml_insert_10col_streaming | 11.9 | 11.4 | 同等 |
| dml_update_10col_streaming | 11.5 | 12.9 | after repeat 3に15.9の外れ値。注意 |
| dml_delete_10col_streaming | 12.2 | 11.8 | 同等 |

`metadata-header --calls=200`:

| measurement | before p50 us | after p50 us | before p99 us | after p99 us | 判断 |
| --- | ---: | ---: | ---: | ---: | --- |
| req_0_resp_0_value_0b | 48.8 | 33.0 | 627.6 | 679.1 | p50改善方向、対象外ノイズも含む |
| req_10_resp_0_value_32b | 45.8 | 37.8 | 298.0 | 664.3 | p50改善方向、p99悪化方向 |
| req_10_resp_10_value_32b | 53.5 | 75.6 | 358.2 | 451.5 | 悪化方向 |
| req_50_resp_0_value_32b | 90.9 | 103.5 | 535.7 | 620.2 | 悪化方向 |
| req_50_resp_50_value_32b | 160.8 | 170.5 | 1005.2 | 1270.1 | 悪化方向 |

追加benchの判断:

- 候補3-5を性能改善として採用する根拠はない。
- `cpu-micro` は多くの代表shapeで同等だが、metadata caseは微悪化方向、`tiny_unary_0b` と `dml_update_10col_streaming` には外れ値がある。
- `metadata-header` はrequest/response metadataを含むケースで悪化方向が見えるため、今回の採用理由はあくまで可読性 / 境界整理に限定する。
- 候補5はstep benchで最も疑わしいため、採用せず戻す。
- 候補3-4は `.text` がほぼ不変で、PHPT / C unit / static analysisは通過している。現時点では明確なregressionとして棄却するほどの結果ではないが、性能改善ではなく可読性改善としてPR上で慎重に判断する。

### 候補3-5 step bench

まとめてbenchした結果だけでは、どの候補が悪化方向に寄せたのか分からない。そのため、隣接コミットごとに追加でbenchを取った。

対象:

- before candidate3: `a46ba3e`
- candidate3: `e427022`
- candidate4: `7d04092`
- candidate5: `cc9ff9b`

`metadata-header --calls=200` のp50:

| measurement | a46ba3e | e427022 | 7d04092 | cc9ff9b | 判断 |
| --- | ---: | ---: | ---: | ---: | --- |
| req_0_resp_0_value_0b | 48.8 | 32.8 | 43.0 | 56.5 | 候補5で悪化方向 |
| req_10_resp_0_value_32b | 45.8 | 37.8 | 44.2 | 56.8 | 候補5で悪化方向 |
| req_10_resp_10_value_32b | 53.5 | 52.0 | 39.9 | 54.9 | 候補5で悪化方向 |
| req_50_resp_0_value_32b | 90.9 | 87.4 | 87.0 | 94.4 | 候補5で悪化方向 |
| req_50_resp_50_value_32b | 160.8 | 170.8 | 157.3 | 172.8 | 候補3と候補5で悪化方向 |

`cpu-micro` のmetadata case:

| commit | metadata_unary_req10_resp10_32b avg cpu_us/call | 判断 |
| --- | ---: | --- |
| a46ba3e | 14.9 | before |
| e427022 | 15.4 | 候補3で微悪化方向 |
| 7d04092 | 15.0 | 候補4で戻る方向 |
| cc9ff9b | 16.2 | 候補5で悪化方向。repeat 1に17.9の外れ値あり |

step benchの判断:

- optimizer remarks上は候補5で `append_grpc_timeout_request_header` の局所costが 65 から -5 に下がるが、実benchでは候補5後にmetadata-header p50が全ケースで悪化方向に振れた。
- 候補4はresponse header callbackに触っているためmetadata-heavy悪化の本命に見えたが、この測定ではp50はむしろ改善方向のケースが多い。一方でp99/maxは荒い。
- 候補3はpersistent preflightで、metadata-header p50では混在。`req50_resp50` とCPU metadata caseでは微悪化方向。
- 現時点で最も疑わしいのは候補5。性能改善目的なら候補5は採用しない方がよい。可読性改善としても、bench riskに見合うほどの価値は弱いため、候補5のコード変更は戻す。
- optimizer remarksの局所cost改善は、実行時性能を保証しない。今回の候補5は、同じsemanticでもcallsite展開、block layout、alignment、I-cache、Docker実行時ノイズの影響を受け、benchでは逆方向に出た可能性がある。

### main vs current after candidate5 revert

候補5を戻した現在HEAD (`232ff94`、code changeは `7991017` まで) と `main` (`116bce6`) で、同じcompose project / test-server / otelopを使ってbenchを取り直した。

`metadata-header --calls=200`:

| measurement | main p50 us | current p50 us | main p99 us | current p99 us | 判断 |
| --- | ---: | ---: | ---: | ---: | --- |
| req_0_resp_0_value_0b | 35.3 | 42.1 | 437.7 | 558.7 | 悪化方向 |
| req_10_resp_0_value_32b | 44.9 | 36.5 | 271.4 | 480.9 | p50改善、p99悪化 |
| req_10_resp_10_value_32b | 55.6 | 47.8 | 1499.9 | 511.2 | 改善方向。ただしmain p99が荒い |
| req_50_resp_0_value_32b | 102.2 | 96.9 | 800.9 | 961.3 | p50改善、p99悪化 |
| req_50_resp_50_value_32b | 157.4 | 148.3 | 1061.6 | 1042.2 | 改善方向 |

`cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3` の代表shape平均:

| measurement | main avg cpu_us/call | current avg cpu_us/call | 判断 |
| --- | ---: | ---: | --- |
| tiny_unary_0b | 11.5 | 11.2 | 同等 |
| small_unary_100b | 10.5 | 11.9 | 悪化方向 |
| new_client_unary_100b | 12.6 | 12.4 | 同等 |
| metadata_unary_req10_resp10_32b | 16.0 | 14.9 | 平均は改善方向。ただしmain repeat 2が18.2で荒い。medianは同等 |
| begin_txn_unary | 10.3 | 10.5 | 同等 |
| commit_txn_unary | 10.1 | 10.3 | 同等 |
| small_streaming_1x100b | 11.0 | 11.0 | 同等 |
| small_streaming_100x100b | 57.5 | 56.4 | 同等〜改善方向 |
| dml_update_10col_streaming | 11.3 | 12.7 | current repeat 2が15.6で荒い。注意 |

main比較の判断:

- 候補5を外した現在HEADは、`metadata-header` p50ではmetadataありケースの多くがmainより改善方向。ただしp99は混在し、`req_0_resp_0` はp50/p99とも悪化方向。
- `cpu-micro` は多くのshapeで同等。metadata CPUは平均では改善方向だが、main側の外れ値に引っ張られておりmedianはほぼ同等。
- この比較でも性能改善として強く主張するのは危険。少なくとも候補5を外したことで、step benchで見えた明確なmetadata-header p50悪化は緩和された。

### main vs current after candidate5 revert: LTO build bench

通常buildだけでは、Clang ThinLTO / GCC LTO のoptimizer remarksを根拠にした変更の検証として不足する。そのため、同じ `main` (`116bce6`) と current (`232ff94`、code changeは `7991017` まで) を、LTO buildでも取り直した。

build条件:

- GCC LTO: `dev` serviceで `CFLAGS="-O2 -flto"` / `LDFLAGS="-flto"` / `make CFLAGS_CLEAN="-O2 -flto -D_GNU_SOURCE"`。
- Clang ThinLTO: `dev-optimizer` serviceで `CC="clang -fuse-ld=lld"` / `CFLAGS="-g -gdwarf-4 -O2 -flto=thin"` / `LDFLAGS="-flto=thin -fuse-ld=lld"` / `make CFLAGS_CLEAN="-g -gdwarf-4 -O2 -flto=thin -D_GNU_SOURCE"`。
- main worktreeは `COMPOSE_PROJECT_NAME=php-grpc-lite` で同じcompose project / test-server / otelopを共有した。
- main branchには `dev-optimizer` serviceがないため、Clang ThinLTOのmain buildだけcurrent側compose定義を使い、main worktreeを `/workspace` にmountしてbuildした。

`metadata-header --calls=200`、GCC LTO:

| measurement | main p50 us | current p50 us | main p99 us | current p99 us | 判断 |
| --- | ---: | ---: | ---: | ---: | --- |
| req_0_resp_0_value_0b | 40.9 | 35.9 | 382.3 | 307.7 | 改善方向 |
| req_10_resp_0_value_32b | 47.0 | 36.8 | 358.8 | 845.9 | p50改善、p99悪化 |
| req_10_resp_10_value_32b | 59.9 | 51.1 | 566.2 | 195.3 | 改善方向 |
| req_50_resp_0_value_32b | 142.0 | 88.4 | 778.2 | 690.8 | 改善方向 |
| req_50_resp_50_value_32b | 160.3 | 158.0 | 991.6 | 1015.7 | ほぼ同等 |

`metadata-header --calls=200`、Clang ThinLTO:

| measurement | main p50 us | current p50 us | main p99 us | current p99 us | 判断 |
| --- | ---: | ---: | ---: | ---: | --- |
| req_0_resp_0_value_0b | 43.0 | 37.6 | 359.9 | 489.6 | p50改善、p99悪化 |
| req_10_resp_0_value_32b | 50.8 | 40.7 | 659.4 | 473.6 | 改善方向 |
| req_10_resp_10_value_32b | 42.5 | 40.2 | 632.5 | 399.7 | 改善方向 |
| req_50_resp_0_value_32b | 99.8 | 96.5 | 1217.7 | 752.0 | 改善方向 |
| req_50_resp_50_value_32b | 164.1 | 155.9 | 2747.5 | 845.3 | 改善方向 |

`cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3` の代表shape平均:

| build | measurement | main avg cpu_us/call | current avg cpu_us/call | 判断 |
| --- | --- | ---: | ---: | --- |
| GCC LTO | tiny_unary_0b | 13.6 | 12.9 | どちらもrepeat 1が荒い |
| GCC LTO | small_unary_100b | 10.7 | 11.6 | 悪化方向 |
| GCC LTO | new_client_unary_100b | 12.7 | 13.5 | 悪化方向 |
| GCC LTO | metadata_unary_req10_resp10_32b | 15.0 | 15.7 | 悪化方向 |
| GCC LTO | small_streaming_100x100b | 57.6 | 57.6 | 同等 |
| Clang ThinLTO | tiny_unary_0b | 12.5 | 11.5 | main repeat 1が荒い |
| Clang ThinLTO | small_unary_100b | 11.0 | 12.6 | current repeat 2が荒い |
| Clang ThinLTO | new_client_unary_100b | 12.7 | 13.0 | ほぼ同等 |
| Clang ThinLTO | metadata_unary_req10_resp10_32b | 15.0 | 15.0 | 同等 |
| Clang ThinLTO | small_streaming_100x100b | 57.8 | 57.9 | 同等 |

LTO比較の判断:

- `metadata-header` のp50は、GCC LTO / Clang ThinLTOともcurrentが全体に改善方向。特にClang ThinLTOではmetadataありケースのp99も改善方向が多い。
- `cpu-micro` はClang ThinLTOでは概ね同等だが、GCC LTOでは小さいunary / metadata unaryが悪化方向に見える。
- optimizer remarks由来の仮説検証としては、通常buildだけよりLTO buildの結果を併記する方が妥当。ただし、CPU代表shapeまで含めると「LTOなら明確に速い」とまでは言えない。採用理由は引き続き可読性 / boundary cleanupを主にし、LTO metadata latencyで改善傾向がある、という限定的な観測に留める。

## 検証

- `docker compose config --services`: `dev-optimizer` serviceを確認。
- `docker compose build dev-optimizer`: PASS。
- `./tools/test/check-c-unit.sh`: PASS。
- `./tools/test/check-c-static-analysis.sh`: PASS。
- `./tools/test/check-phpt.sh`: PASS。15/15。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-header-unchecked`: PASS。
- `./bench/run.sh metadata-header --calls=30`: PASS。afterのみ。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/before-header-unchecked`: PASS。baseline `df5bce2`。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-header-unchecked-v2`: PASS。after `f22189e`。
- `./bench/run.sh metadata-header --calls=200`: PASS。before run id `hotpath-before-metadata`, after run id `hotpath-after-metadata`。
- `./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3`: PASS。before run id `hotpath-before-cpu`, after run id `hotpath-after-cpu`。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-data-callback-early-return`: PASS。
- `./tools/test/check-c-unit.sh`: PASS。candidate2後。
- `./tools/test/check-phpt.sh`: PASS。15/15。candidate2後。
- `./tools/test/check-c-static-analysis.sh`: PASS。candidate2後。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-preflight-probe-split`: PASS。
- `./tools/test/check-c-unit.sh`: PASS。candidate3後。
- `./tools/test/check-phpt.sh`: PASS。15/15。candidate3後。
- `./tools/test/check-c-static-analysis.sh`: PASS。candidate3後。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-header-callback-state-split`: PASS。
- `./tools/test/check-c-unit.sh`: PASS。candidate4後。
- `./tools/test/check-phpt.sh`: PASS。15/15。candidate4後。
- `./tools/test/check-c-static-analysis.sh`: PASS。candidate4後。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-grpc-timeout-unchecked-append`: PASS。
- `./tools/test/check-c-unit.sh`: PASS。candidate5後。
- `./tools/test/check-phpt.sh`: PASS。15/15。candidate5後。
- `./tools/test/check-c-static-analysis.sh`: PASS。candidate5後。
- `./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3`: PASS。candidate3-5 before run id `hotpath-candidate3-5-before-cpu`、after run id `hotpath-candidate3-5-after-cpu`。
- `./bench/run.sh metadata-header --calls=200`: PASS。candidate3-5 before run id `hotpath-candidate3-5-before-metadata`、after run id `hotpath-candidate3-5-after-metadata`。
- `./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3`: PASS。candidate3 run id `hotpath-step-c3-cpu`、candidate4 run id `hotpath-step-c4-cpu`、candidate5 run id `hotpath-step-c5-cpu`。
- `./bench/run.sh metadata-header --calls=200`: PASS。candidate3 run id `hotpath-step-c3-metadata`、candidate4 run id `hotpath-step-c4-metadata`、candidate5 run id `hotpath-step-c5-metadata`。
- `./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3`: PASS。main run id `hotpath-main-cpu`、current run id `hotpath-current-no-c5-cpu`。
- `./bench/run.sh metadata-header --calls=200`: PASS。main run id `hotpath-main-metadata`、current run id `hotpath-current-no-c5-metadata`。
- GCC LTO build: PASS。main/currentとも `php -d extension=/workspace/modules/grpc.so` load確認済み。
- `./bench/run.sh metadata-header --calls=200`: PASS。GCC LTO main run id `hotpath-gcc-lto-main-metadata`、current run id `hotpath-gcc-lto-current-no-c5-metadata`。
- `./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3`: PASS。GCC LTO main run id `hotpath-gcc-lto-main-cpu`、current run id `hotpath-gcc-lto-current-no-c5-cpu`。
- Clang ThinLTO build: PASS。main/currentとも `php -d extension=/workspace/modules/grpc.so` load確認済み。
- `./bench/run.sh metadata-header --calls=200`: PASS。Clang ThinLTO main run id `hotpath-clang-thinlto-main-metadata`、current run id `hotpath-clang-thinlto-current-no-c5-metadata`。
- `./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3`: PASS。Clang ThinLTO main run id `hotpath-clang-thinlto-main-cpu`、current run id `hotpath-clang-thinlto-current-no-c5-cpu`。

## 判断ログ

- 2026-06-07: `ZEND_VM_KIND_TAILCALL` はZend VM executor dispatchの話であり、php-grpc-lite拡張Cコードの直接最適化とは別レイヤーと整理した。
- 2026-06-07: `always_inline` ではなく、まずearly return / slow path splitで通常optimizerが読みやすい形になるかを見る。
- 2026-06-07: 今回のunchecked append splitはremarks上の局所costを下げるが、関数全体のinline可否は変わらない。benchもafter単独なので、この時点では採用確定ではなくtrial結果として扱う。
- 2026-06-07: 同条件before/afterを取った結果、局所的なinline改善は確認できたが、DSO `.text` とmetadata CPU benchでは性能改善として主張できる差は見えなかった。採用理由は性能値ではなく、prechecked pathの責務分離で見通しがよくなることに置く。
- 2026-06-07: 調査を広げるなら、metadata pathよりもSpanner形状で支配的なserver streaming / response processing / persistent connection preflight周辺を優先する。
- 2026-06-07: `on_data_chunk_recv_callback` のearly return化はDSO / remarks上の性能改善はないが、DATA chunk callbackの責務分岐が読みやすくなり、subagent protocol checkとPHPTでsemantics維持を確認したため採用する。
- 2026-06-07: optimizer remarksを根拠にするなら通常build benchだけでは不足するため、GCC LTO / Clang ThinLTO buildでもmain/currentを比較した。LTO metadata latencyではcurrent改善傾向があるが、CPU代表shapeでは同等〜一部悪化もあるため、性能改善PRとしては扱わない。
- 2026-06-07: `preflight_persistent_connection` のTLS / plain socket probe分離は、DSO / remarks上の性能改善はない。むしろ `.text` は +72 bytes、`get_persistent_connection` へのinline costは 760 から 820 へ増えた。一方で、persistent reuse入口のlifecycle条件は読みやすくなり、drain処理とerror taxonomyを変えないことをsubagent reviewとPHPTで確認したため、可読性改善として採用する。
- 2026-06-07: `on_header_callback` のheader state分離は `.text` 不変で、重いdecode / metadata保存helperのinline可否も変わらない。trailing分類とspecial header side effectの順序が明示され、subagent reviewとPHPTでsemantics維持を確認したため、可読性改善として採用する。
- 2026-06-07: `append_grpc_timeout_request_header` のunchecked append化は、対象callsiteの局所costを 65 から -5 に下げるが、`.text` は +4 bytesで性能改善根拠にはならない。owned value登録後のcapacity確認とunchecked appendという責務は明確になるが、可読性差分は小さい。
- 2026-06-07: 候補3-5のbenchを追加で取得した。`cpu-micro` は多くのshapeで同等だが、metadata caseは微悪化方向。`metadata-header` はmetadataありケースで悪化方向が見える。したがって候補3-5は性能改善ではなく、可読性改善としても採用判断をPR上で慎重に扱う。
- 2026-06-07: 候補3-5をstep benchで分解した。最も疑わしいのは候補5で、局所optimizer costは改善しているがmetadata-header p50は全ケースで悪化方向に振れた。候補5は採用価値が弱いため、コード変更を戻す。
- 2026-06-07: 候補5を外した現在HEADとmainを比較した。metadata-header p50ではmetadataありケースの多くが改善方向だが、p99は混在し、cpu-microは概ね同等。性能改善PRとは扱わず、可読性改善PRとしてbench riskを添えて判断する。

## 完了条件

- 対象候補、before/after、採用/棄却判断を記録する。
- 採用する場合は検証結果と副作用を記録する。
- 採用しない場合も、なぜ見送るかを記録してissueを閉じる。
