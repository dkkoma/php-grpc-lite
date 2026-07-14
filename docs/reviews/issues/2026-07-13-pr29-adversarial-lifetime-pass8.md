# PR #29 第八パス C lifetime / destroy-trace review 2026-07-13

## Scope

- `199bf01..1faf80a`（対応commit `3ed170f`、review record commit `1faf80a`）
- `src/transport.c` の `destroy_h2_connection()` / trace cache / persistent cache teardown
- `src/unary_call.c` のmem-recv fatal cleanup
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`
- `grpc.c` のMINIT / MSHUTDOWN / GINIT / GSHUTDOWN順序
- `tests/phpt/029-trace-file.phpt` と既存wire trace consumer

## Reviewer Role

- `low-level C lifetime / destructor and trace-oracle adversary`

## Review Prompt Summary

- `wire.connection_close`追加について、destructor実行との対応、trace I/Oとresource解放の順序、reentrancy / double-destroy / errno依存、ZTSを含むmodule lifecycle、production / diagnostic境界を確認した。
- PHPT 041の132件preface / close cardinalityが、同一process内の接続生成・破棄数と一致し、前パスのdetach-only mutationを検出するか確認した。
- close eventを追加しても残るmutation gapを、前パスで要求した「mem-recv fatal branchからdestroy callが欠落する回帰」のスコープと区別して評価した。

## Issues

- none

## Verification Notes

- `git diff --check 199bf01..1faf80a`: PASS。
- `destroy_h2_connection()`はNULL guard後にclose eventを1回記録し、その後のSSL、SSL_CTX、fd、write / recv buffer、nghttp2 session / callbacks、connection allocation解放までearly returnを持たない。trace fileのopen / lockに失敗してもcleanupへ進み、event処理からtransport destructorへ再入するcallbackはない。
- eventはconnectionを解放する前に`fd` / `dead`を読むため、freed memory参照はない。lockはevent内で解除されてからSSL / nghttp2 destructorへ進み、session deleteもstream callbackを起動しない既存lifecycle前提と整合する。
- production buildでもevent codeは存在するが、既存のprocess diagnosticである`GRPC_LITE_TRACE_FILE`がMINIT時に設定されていない通常経路はpointerのNULL checkだけで、test-fault / bench functionやbuild optionを新たに露出しない。trace pathはread-only process snapshotで、ZTS writer間は既存のfile lockを共有する。
- module shutdownではMSHUTDOWNがtrace path cacheを解放し、後続のglobals teardownから呼ばれるdestroyはeventをskipしてresource解放だけ行う。このため`wire.connection_close`はprocess全期間のallocation balance APIではないが、PHPT 041が対象とするrequest中のfailure-consumed connectionはcoreから戻る前に同期destroyされ、testがtraceを読む前に全eventが確定する。今回のoracle用途には欠落しない。
- PHPT 041のbodyが作るh2 connectionは、same-key 2 attemptとdistinct-authority 130 attemptの計132個だけである。全attemptはexact `nghttp2_session_mem_recv failed`を要求するためsetup failureやcache exhaustionでは継続できず、preface 132件はfresh connection、close 132件は各fatal branchからのdestroy helper実行を独立に固定する。SKIPIFのreachability probeは別PHP processかつraw `fsockopen`でありcountへ混入しない。
- 前パスで問題にしたmutation（mem-recv FAILURE branchの`destroy_detached_connection_if_unowned()`呼出しだけを除去）はclose eventを0件にするため、追加assertで検出される。`destroy_h2_connection()`内部の個別release文とeventを意図的に乖離させるmutationまですべてallocator-independentに証明するものではないが、それは今回変更したcallsite ownership oracleの要求を超える。現実装の各releaseと順序に欠落はない。
- `wire.connection_close`は既存trace consumerがevent名でfilterするJSONLへ追加され、PHPT 029やbenchmark集計は未知eventの有無・総行数へ依存しない。PHPT 041自身もevent名だけをcountするため、記録済み`fd`のOS再利用には依存しない。

## Cross-review Triage

- **PRへ投稿推奨:** domain pass8 `REVIEW-20260713-007`。新設eventは現在の実装でlocal destructor開始時に発火し、TCP/TLS/HTTP/2のwire closeを観測していないため、`wire.connection_close`は観測domainを誤る。今回のdestroy oracle追加そのものから直接生じたcurrent trace-semantics defectで、`transport.connection_destroy`等へのrenameは元issueのconnection lifecycle scope内に収まる。
- **PRへ投稿推奨:** domain pass8 `REVIEW-20260713-008`。元issueの28行目は現在形で「streaming deadlineだけ接続破棄」と残り、同じ文書のcurrent-state / Progress / SPECと正面から矛盾する。今回対応した文書整合の見落としであり、元issueの中心scopeそのものなのでLowとして強い。
- **今回の投稿から除外:** build pass8 `REVIEW-20260713-007`。現行production/release artifactのflagsとsource boundaryは正しく、pure-production sanitizer laneも手動gateとして実行済みである。Native QA / release workflowへ追加する提案は有用だが、現在のdeadline/RST実装のcorrectness defectではなくrepository-wide CI hardeningである。
- **今回の投稿から除外:** build pass8 `REVIEW-20260713-008`。productionではfault parser / MINIT stateがcompile outされpredicateもconstant falseで、MINFOも同じcompile macroに従う。guardを意図的に分離する将来mutation向けnegative behavior probeは追加hardeningであり、現在のboundary defectではない。
- **今回の投稿から除外:** build pass8 `REVIEW-20260713-009`。bench-only (`1/0`) laneは独立option matrixとして望ましいが、今回のfault-driven lifetime regressionはbench+fault (`1/1`)で初めて到達し、production (`0/0`)も別laneで検証済みである。未使用variantの網羅は元issueを越える一般的build-matrix拡張である。
- 上記domain 2件はC lifetimeのcurrent safety defectを示すものではないため、本review自身のseverity countは変更しない。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
