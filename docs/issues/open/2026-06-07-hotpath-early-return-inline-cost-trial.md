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

この候補は、custom metadata pathと同じくprechecked appendを明示する整理であり、性能改善とは主張しない。`.text` は +4 bytesで実質不変。`grpc-timeout` value lifetimeを変えず、subagent reviewでも登録済みvalueは `free_request_headers()` 解放に任せる方針が妥当と確認した。

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

## 判断ログ

- 2026-06-07: `ZEND_VM_KIND_TAILCALL` はZend VM executor dispatchの話であり、php-grpc-lite拡張Cコードの直接最適化とは別レイヤーと整理した。
- 2026-06-07: `always_inline` ではなく、まずearly return / slow path splitで通常optimizerが読みやすい形になるかを見る。
- 2026-06-07: 今回のunchecked append splitはremarks上の局所costを下げるが、関数全体のinline可否は変わらない。benchもafter単独なので、この時点では採用確定ではなくtrial結果として扱う。
- 2026-06-07: 同条件before/afterを取った結果、局所的なinline改善は確認できたが、DSO `.text` とmetadata CPU benchでは性能改善として主張できる差は見えなかった。採用理由は性能値ではなく、prechecked pathの責務分離で見通しがよくなることに置く。
- 2026-06-07: 調査を広げるなら、metadata pathよりもSpanner形状で支配的なserver streaming / response processing / persistent connection preflight周辺を優先する。
- 2026-06-07: `on_data_chunk_recv_callback` のearly return化はDSO / remarks上の性能改善はないが、DATA chunk callbackの責務分岐が読みやすくなり、subagent protocol checkとPHPTでsemantics維持を確認したため採用する。
- 2026-06-07: `preflight_persistent_connection` のTLS / plain socket probe分離は、DSO / remarks上の性能改善はない。むしろ `.text` は +72 bytes、`get_persistent_connection` へのinline costは 760 から 820 へ増えた。一方で、persistent reuse入口のlifecycle条件は読みやすくなり、drain処理とerror taxonomyを変えないことをsubagent reviewとPHPTで確認したため、可読性改善として採用する。
- 2026-06-07: `on_header_callback` のheader state分離は `.text` 不変で、重いdecode / metadata保存helperのinline可否も変わらない。trailing分類とspecial header side effectの順序が明示され、subagent reviewとPHPTでsemantics維持を確認したため、可読性改善として採用する。
- 2026-06-07: `append_grpc_timeout_request_header` のunchecked append化は、対象callsiteの局所costを 65 から -5 に下げるが、`.text` は +4 bytesで性能改善根拠にはならない。owned value登録後のcapacity確認とunchecked appendという責務が明確になるため、見通し改善として採用する。

## 完了条件

- 対象候補、before/after、採用/棄却判断を記録する。
- 採用する場合は検証結果と副作用を記録する。
- 採用しない場合も、なぜ見送るかを記録してissueを閉じる。
