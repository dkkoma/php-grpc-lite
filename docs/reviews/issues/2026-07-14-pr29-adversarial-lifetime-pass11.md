# PR #29 第十一パス C lifetime / connection lifecycle review 2026-07-14

## Scope

- 前回レビュー済みHEAD `f94d3ba4dd95a8496b1d32efb372079b1ac89242`
- 第十パス対応commit / 現在HEAD `b7b9fd13b177d2426cb07c28f1f12717cc670e6d`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md` の `REVIEW-20260714-002`
- `src/transport.c` のpreflight drain、dead / draining、cache detach / destroy、fresh connection adoption
- `grpc.c` / `src/transport_core.h` のpreflight drain cap default / effective値
- `poc/test-server/main.go` のbacklog flood fixtureとTCP arrival barrier

## Reviewer Role

- C lifetime / HTTP/2 connection cache・dead / draining lifecycle adversary

## Review Prompt Summary

- 第十パスLowへのコメント修正が、設定済みcap、production default、receive backlog、TCP arrival barrier、adoption時点、draining / dead、fresh connection、owner lifecycleを実装どおり表すか静的に再監査した。
- 元issueのdeadline / RST_STREAM / best-effort reuseに必要な範囲だけfull PRのlifecycleを再確認し、generic CI / styleへはscopeを広げていない。
- production codeは変更していない。

## Issues

### Blocker

- none

### High

- none

### Medium

- none

### Low

- none

### Design Decision

- none

## Verification Notes

- `b7b9fd1` のruntime差分はなく、PHPT 035の説明コメントと中央review recordだけが変わった。connection / stream state、cache entry、owner count、fd / TLS / nghttp2 sessionのlifetimeは前回レビュー済みHEADから不変である。
- `REVIEW-20260714-002` はadequateに修正された。PHPTは `--INI--` で `grpc_lite.preflight_drain_max_bytes=16384` を設定し、`effective_preflight_drain_max_bytes()` のminimum 4096を上回るためeffective capも正確に16KiBとなる。production defaultは `grpc.c` と `GRPC_LITE_PREFLIGHT_DRAIN_DEFAULT_MAX_BYTES` の65536 bytesであり、新コメントはtest設定値とdefaultを明確に区別する。
- fixtureは3個の16KiB DATA payloadを送る。serverの `SO_SNDBUF` を4KiBへ縮小し、全 `WriteData()` 完了後に別control connectionへ `ready` を返すため、最後の少量を除くbacklogがclient TCP receive側へ到着済みというbarrierになる。clientは `ready` を受けるまでdata connectionを再読込せず、48KiB payloadはconfigured 16KiB capを十分に超える一方、このtestがproduction 64KiB境界自体を跨いでいないという新コメントも正しい。
- `$call->cancel()` は対象streamへRST_STREAMを送り、server fixtureはRST受信後もconnectionを開いたままにする。cancel cleanupでstream registrationとownerを解除した後、follow-up callの `get_persistent_connection()` がcache内candidateをadoptする直前にpreflightを実行する。したがってコメントはbacklog形成時点やactive stream中にpreflightが走るとは主張していない。
- preflightはread lengthを残capに制限するため、このケースでは累計16384 bytesでloopを抜け、read boundary未確認のまま `mark_connection_draining(..., NGHTTP2_NO_ERROR)` へ遷移する。これはconnection failureを表す `dead` ではなく、新規stream admissionを禁止するbest-effort fallbackであり、新コメントの `(draining)` は実装どおりである。
- `get_persistent_connection()` はfalseを返したpreflight candidateを `remove_unusable_persistent_connection()` でcacheから除外する。このPHPTではcancel済みcallのowner countが0なので旧connectionをdestroyし、その後unarmed fixture向けに新しいconnectionを作成・cache登録する。`persistent_reused=false` とconnection preface 2件のassertionがfresh connection semanticsを直接固定している。新コメントはpreflight helper単体がsocketを開くというAPI責務を要求するものではなく、adoption orchestration全体の結果を述べている。
- 「follow-up call must succeed either way」は、reuseがbest-effortでfallbackしてもRPC結果を保つというsemantic contractを述べる。直後のassertionはこのfixture条件ではfresh fallbackを必須としており、fresh / reusedのどちらでもtestを通すというoracleにはなっていない。
- full PRの関連lifecycleも再確認した。deadは全ownerにterminal、drainingは既にadmitされたstreamのI/O / RSTを許可しつつ新規admissionを拒否し、cacheからdetachされたconnectionは最後のowner clearまで物理destroyを遅延する。今回の文書修正およびREVIEW-20260714-002のFix summaryと矛盾する残件はない。
- 中央review recordのFinding / Expected model / Fix summaryは、旧コメントが16KiBのtest capを64KiB defaultと誤記していた事実と今回の修正範囲を正確に履歴化している。過去行を現在のruntime contractとして誤読させる追加矛盾はない。
- `git diff --check f94d3ba4dd95a8496b1d32efb372079b1ac89242..b7b9fd13b177d2426cb07c28f1f12717cc670e6d`: PASS。
- `git diff --check main...HEAD`: PASS。
- build / test: 親レビューの検証laneと重複させず、このroleではstatic lifecycle reviewのみ実施。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
