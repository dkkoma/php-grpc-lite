# ZTS formal support review 2026-06-01

## Scope

- `grpc.c`
- `src/module.h`
- `src/transport.c`
- `src/wrapper_adapter.c`
- `tools/test/check-zts-phpt.sh`
- `tools/test/check-zts-performance.sh`
- `tools/test/check-zts-parallel-performance.sh`
- `.github/workflows/native-qa.yml`
- `docs/issues/open/2026-05-28-zts-formal-support.md`

## Reviewer Role

- ZTS / PHP extension lifecycle reviewer

## Review Prompt Summary

- ZTS正式サポート前の追加確認タスクとして、process-wide state、module globals、persistent connection cache、diagnostic env、FrankenPHP worker経路、NTS/ZTS QA gateを確認した。

## Issues

### REVIEW-20260601-001: SIGPIPE policy is process-wide by design

- Severity: Design Decision
- Status: Accepted
- Reviewer role: ZTS / PHP extension lifecycle reviewer
- Finding: `grpc.c` の `signal(SIGPIPE, SIG_IGN)` はZTSでもprocess-wide設定であり、thread-localではない。
- Evidence: `grpc.c` `PHP_MINIT_FUNCTION(grpc_lite)`, `src/transport.c` `h2_connection_send()`
- Expected model: threaded SAPIではprocess-wide状態変更を暗黙にせず、維持する場合はmodule-level policyとして明示する。
- Why it matters: TLS write pathはplatformごとのper-socket SIGPIPE抑止が揃わないため、process-wide ignoreを外すとremote close時のprocess termination riskが残る。一方で、ZTS利用者にはprocess全体へ影響する判断であることが見えている必要がある。
- Recommended fix: process-wide policyとしてコメントとissueへ記録する。plain socket writeは引き続き `MSG_NOSIGNAL` / `SO_NOSIGPIPE` を使う。
- Fix summary: `grpc.c` にZTS policy commentを追加した。
- Fix commit: closeout commit
- Verification: code review

### REVIEW-20260601-002: diagnostic env vars are process diagnostics

- Severity: Design Decision
- Status: Accepted
- Reviewer role: ZTS / PHP extension lifecycle reviewer
- Finding: `GRPC_LITE_TRACE_FILE` / `GRPC_LITE_TRACE_WIRE_BYTES` はruntime `getenv()` を読むため、ZTSではprocess environmentに依存する。
- Evidence: `src/wrapper_adapter.c` `grpc_lite_trace_enabled()` / `grpc_lite_trace_record_call()`, `src/transport.c` `grpc_lite_trace_file_path()` / `grpc_lite_trace_wire_bytes_enabled()`
- Expected model: trace機能はproduction configurationではなく、process-wide opt-in diagnosticとして扱う。request/threadごとの動的切替を正式APIにしない。
- Why it matters: ZTS thread間で環境変数を動的に変更する使い方は安全な運用モデルではない。trace file自体はappend時にlockされるが、設定値はprocess-wideとして読む必要がある。
- Recommended fix: ZTS policy commentとissueへ記録する。per-request traceが必要になった場合はINI/module globalsまたは明示APIを別issueで設計する。
- Fix summary: trace env readerにZTS policy commentを追加した。
- Fix commit: closeout commit
- Verification: code review

### REVIEW-20260601-003: persistent connection cache remains thread-local

- Severity: Design Decision
- Status: Accepted
- Reviewer role: ZTS / PHP extension lifecycle reviewer
- Finding: persistent connection cacheはmodule globals上の `HashTable` で、ZTSではthread-local module globalsとして扱われる。
- Evidence: `src/module.h` `PHP_GRPC_LITE_G(persistent_connections)`, `grpc.c` `PHP_GINIT_FUNCTION(grpc_lite)`, `src/transport.c` persistent connection lookup/update paths
- Expected model: `h2_connection`、socket、`nghttp2_session`、`SSL*` はthreadをまたいで共有しない。各threadは自分のmodule globals上のpersistent cacheだけを使う。
- Why it matters: HTTP/2 sessionやOpenSSL objectを複数threadで共有すると、Zend API境界、socket ownership、nghttp2 session stateの同期が必要になる。現行設計はそれを行わない。
- Recommended fix: thread-local invariantをコードコメントと設計文書へ残す。ZTS thread並列QAでPHP userland call pathを通す。
- Fix summary: `src/module.h` にthread-local invariant commentを追加した。設計文書は既に `docs/SPEC.md` と `docs/design/http2-transport-design.md` に記載済み。
- Fix commit: closeout commit
- Verification: `./tools/test/check-zts-parallel-performance.sh`

### REVIEW-20260601-004: FrankenPHP worker comparison is a QA evidence, not an alternate backend

- Severity: Design Decision
- Status: Accepted
- Reviewer role: ZTS / PHP extension lifecycle reviewer
- Finding: FrankenPHP ZTSはnative transportのthreaded SAPI利用を確認するためのapplication pathであり、runtime transport追加ではない。
- Evidence: `compose.yaml` `franken-zts-laravel-native`, `Dockerfile.franken-zts`, `docs/SPEC.md` runtime transport方針
- Expected model: runtime transportはnghttp2 1系統のまま、FrankenPHPはZTS worker SAPIで同一extensionをloadする検証対象として扱う。
- Why it matters: 以前削除したfranken-go backendやtransport selectionをZTS正式サポートに混ぜると、設計方針が再び曖昧になる。
- Recommended fix: issueの判断ログに、FrankenPHP ZTSは検証対象であり別transportではないことを残す。
- Fix summary: issue closeoutで明記する。
- Fix commit: closeout commit
- Verification: issue review

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: 4
