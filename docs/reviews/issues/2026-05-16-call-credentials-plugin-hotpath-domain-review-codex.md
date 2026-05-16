# CallCredentials plugin hotpath domain review 2026-05-16

## Scope

- `ext/grpc/bridge.c`
- `ext/grpc/internal.h`
- `ext/grpc/surface.c`
- `ext/grpc/tests/023-metadata-and-call-credentials.phpt`
- `tools/benchmark/cpu-micro.php`
- `tools/benchmark/StreamingBenchHelper.php`

## Reviewer Role

- `CallCredentials / gRPC domain model reviewer`

## Review Prompt Summary

- CallCredentials plugin hotpath changeについて、gRPC CallCredentials semantics、service_url / method_name互換性、C/PHP zval lifecycle、Channel-owned cache lifecycle、benchmark/test boundary、service-level metadata generationとper-RPC callback semanticsの分離を確認した。

## Issues

### REVIEW-20260516-001: service_url が effective authority ではなく raw target から作られる

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `CallCredentials / gRPC domain model reviewer`
- Finding: `grpc_lite_get_call_credentials_service_url()` は service URL を `scheme://channel->target + service` としてcacheしているが、このissueのdomain modelは service URL を `scheme://authority/package.Service` として扱う前提であり、`grpc.default_authority` があるChannelでは wire authority と CallCredentials audience が乖離する。
- Evidence: `ext/grpc/bridge.c` / `grpc_lite_get_call_credentials_service_url()` は `ZSTR_VAL(channel->target)` を使う。`ext/grpc/surface.c` / `Channel::__construct()` は `grpc.default_authority` を `channel->authority` として保持し、connection key / HTTP/2 authority側には反映している。`docs/issues/open/2026-05-16-call-credentials-plugin-hotpath.md` は service URL を `scheme://authority/package.Service` と明記している。
- Expected model: CallCredentials pluginに渡す `serviceUrl` は gRPC service単位の認可audienceであり、Channelのeffective authority（`grpc.default_authority` があればそれ、なければtarget authority）から生成されるべき。`methodName` は引き続きRPC単位のフルメソッド `/package.Service/Method` を渡す。
- Why it matters: ADC / google-auth のmetadata pluginは service URL をaudienceやcache keyとして扱いうるため、targetとauthorityが異なるprivate endpoint、override、proxy構成で公式grpc互換の認可metadataを生成できない可能性がある。Channel-owned cache自体は妥当でも、cacheする値のdomain sourceが誤る。
- Recommended fix: service URL生成用に `channel->authority` が設定されていればそれを使い、なければtarget authority（host:portを含む既存target互換値）を使う helper を追加する。PHPTに `grpc.default_authority` ありのCallCredentials callback確認を追加し、`serviceUrl` はauthority、`methodName` はフルメソッドのまま維持することを固定する。
- Fix summary: `service URL生成で channel->authority を target より優先し、grpc.default_authorityありのPHPTを追加した。`
- Fix commit: `this commit`
- Verification: `./tools/test/check-phpt.sh`, `./tools/test/check-c-static-analysis.sh`
- Notes: cache keyをservice pathにする判断はservice-level cacheとして自然だが、cache valueのauthority sourceを修正する必要がある。

### REVIEW-20260516-002: callback zvalをpinせずにuserland callbackへ渡している

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `CallCredentials / gRPC domain model reviewer`
- Finding: hotpath変更で `credentials->callback` の `ZVAL_COPY()` / dtorが削除され、`call_user_function()` に `&credentials->callback` を直接渡している。CallCredentials callbackはuserland任意コードなので、実行中に同じ `Grpc\Call` のcredentials差し替えや参照解放が起きても、呼び出し対象zvalのlifetimeが独立している必要がある。
- Evidence: `ext/grpc/bridge.c` / `grpc_lite_merge_call_credentials_metadata()` は `credentials = Z_GRPC_LITE_CALL_CREDENTIALS_P(&call->credentials)` 後、`call_user_function(..., &credentials->callback, ...)` を呼ぶ。`ext/grpc/surface.c` / `Call::setCredentials()` は `zval_ptr_dtor(&obj->credentials)` してから新しいcredentialsをcopyするため、re-entrant userlandから到達できると古い `Grpc\CallCredentials` objectとその `callback` propertyが破棄されうる。
- Expected model: C/PHP bridgeがuserland callableを実行する間は、callback zvalのlifetimeはCallCredentials objectやCall objectのre-entrant mutationから独立してpinされるべき。per-RPC callback semanticsを保つため、callback呼び出し自体は毎RPC実行しつつ、呼び出し対象のzval安全性はC bridgeが保証する。
- Why it matters: 通常のGAX経路では顕在化しにくいが、CallCredentials pluginは任意PHPコードであり、closure captureやglobal stateから同じCallを操作できる。callback zvalをpinしない最適化は、性能hotpathのためにZend object / zval lifecycle invariantを弱め、use-after-free系の不安定化を再導入するリスクがある。
- Recommended fix: `call_user_function()` 前にcallback zvalをlocal `function_name` へ `ZVAL_COPY()` して呼び出し中だけpinし、全exit pathでdtorする。固定費削減が必要なら、copy削除ではなくservice URL cacheやcallable cacheなどlifetimeが明確な別施策に限定する。可能ならre-entrant callbackで `setCredentials()` するPHPTを追加する。
- Fix summary: `callback zvalをlocal function_nameへZVAL_COPYしてcall_user_function中にpinする形へ戻した。`
- Fix commit: `this commit`
- Verification: `./tools/test/check-phpt.sh`, `./tools/test/check-c-static-analysis.sh`
- Notes: service URL文字列cacheはChannel-owned cache lifecycleとして妥当だが、callback callableのownershipはCallCredentials object内propertyであり、userland実行中のpinningが別に必要。

### REVIEW-20260516-003: cpu-micro のCallCredentialsケースがTLS fixture依存を無条件に広げている

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `CallCredentials / gRPC domain model reviewer`
- Finding: `cpu-micro.php` はCallCredentials hotpath観測のためにTLS target `test-server:50052`、cert fixture、TLS clientsを無条件に初期化するようになった。これはCallCredentials plugin bridge固定費のbenchmarkにTLS fixture availability / hard-coded targetという別domainの前提を混ぜている。
- Evidence: `tools/benchmark/cpu-micro.php` は起動時に常に `poc/test-server/certs/server.crt` を読み、`test-server:50052` のTLS clientsを作る。CallCredentials casesだけが `secure => true` を持つが、suite全体の初期化時点でTLS fixtureが必須になる。`--target` で指定したbenchmark targetもCallCredentials casesには反映されない。
- Expected model: benchmark/test boundaryでは、CallCredentials bridge固定費を測るcaseはRPC callback metadata生成の有無を主変数にし、TLS availabilityや別port固定は明示的なsecure benchmark caseとして分離するべき。汎用cpu-micro suiteのtarget optionはcase contextと実際のclient作成に一貫して効くべき。
- Why it matters: TLS接続自体はreusedでも、suite依存関係と失敗条件が増えるとCPU micro benchmarkのproduction hotpath観測が不安定になり、CallCredentials改善とTLS環境差の責務境界が曖昧になる。custom targetでの比較やext-grpc比較でも、表示されたtargetと実際のCallCredentials targetがずれる可能性がある。
- Recommended fix: CallCredentials CPU casesを既存target上で走らせるか、`--tls-target` / `--call-credentials-secure` のような明示option付きの別caseとして初期化を遅延する。少なくともTLS fixture read/client作成はsecure CallCredentials case実行時だけにし、OTEL contextにsecure targetとcredentials modeを明記する。
- Fix summary: `TLS fixture readとsecure client作成をsecure CallCredentials case実行時へ遅延し、OTEL contextにcall_credentials種別を記録した。`
- Fix commit: `this commit`
- Verification: `BENCH_IMPLEMENTATION=php-grpc-lite ./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100`
- Notes: secure channel上でのみCallCredentialsを測る判断自体はあり得るが、その場合はcpu-microの通常境界から独立したDesign Decisionとして記録するとよい。

## Review Result

- Blocker: `none`
- High: `0`
- Medium: `0`
- Low: `none`
- Design Decision: `none`

## Re-review 2026-05-16

- Result: `previous findings remain fixed; no new findings`
- Checked: `effective authority in service_url`, `callback zval pinning during userland invocation`, `cpu-micro secure CallCredentials lazy TLS initialization`
- Evidence: `ext/grpc/bridge.c` now prefers `channel->authority` for service URL and pins the callback via local `function_name`; `ext/grpc/tests/023-metadata-and-call-credentials.phpt` covers authority override; `tools/benchmark/cpu-micro.php` lazily initializes TLS clients only for secure CallCredentials cases and records per-case CallCredentials context.
