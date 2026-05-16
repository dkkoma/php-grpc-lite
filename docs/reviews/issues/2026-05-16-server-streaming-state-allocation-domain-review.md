# server streaming state allocation domain review 2026-05-16

## Scope

- `ext/grpc/internal.h`
- `ext/grpc/server_streaming_call.c`
- `ext/grpc/transport.c`
- `docs/issues/open/2026-05-16-server-streaming-state-allocation-hotpath.md`

## Reviewer Role

- HTTP/2/gRPC domain model and PHP extension lifecycle reviewer

## Review Prompt Summary

- `server_streaming_call_state` から `path` と `metadata` を削除する未コミット変更について、diagnostic/cleanup/lifecycle/ownership/modeling 上の安全性を確認する。

## Issues

### REVIEW-20260516-001: bench diagnostic path still depends on server streaming method path

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `HTTP/2/gRPC domain model and PHP extension lifecycle reviewer`
- Finding: `server_streaming_call_state.path` の削除後も bench diagnostic の server streaming status record が `state->path` を参照している。通常production buildでは `diagnostic.c` は含まれないが、`--enable-grpc-bench` buildではコンパイル不能になるか、diagnostic record の `rpc_service` / `rpc_method` を作れない。
- Evidence: `ext/grpc/diagnostic.c` の `grpc_lite_diagnostic_build_server_streaming_record()` は `state->path == NULL` を確認し、`grpc_lite_diagnostic_split_method(ZSTR_VAL(state->path), ZSTR_LEN(state->path), ...)` を呼ぶ。一方、今回の変更で `ext/grpc/internal.h` の `server_streaming_call_state` から `path` が削除され、`ext/grpc/server_streaming_call.c` の `zend_string_init(path, path_len, 0)` と `ext/grpc/transport.c` の release も削除されている。
- Expected model: gRPC method path は request submission 時の call semantic stateであり、production stream lifecycle には不要なら保持しない。一方、bench diagnostic が `rpc_service` / `rpc_method` を出すなら、diagnostic-only state として明示的に所有するか、diagnostic helper へ別経路で渡す必要がある。production state削減と diagnostic record生成の責務境界を壊さない。
- Why it matters: ベンチ/diagnostic build は性能観測の一次経路であり、production codeから切り離されていてもサポート対象の開発用extensionである。未使用field削除というhotpath改善が diagnostic boundary を壊すと、Spanner mixed などの検証・回帰観測ができなくなる。
- Recommended fix: `metadata` 削除は維持しつつ、`path` だけは `#ifdef PHP_GRPC_LITE_ENABLE_BENCH` で `server_streaming_call_state` に保持して diagnostic dtor で解放する、または server streaming diagnostic status helper の入力に method path を明示的に渡す設計へ変更する。あわせて issue本文の「diagnostic用にも参照していない」を修正する。
- Fix summary: `metadata` は完全に削除し、`path` は production state から外したうえで `PHP_GRPC_LITE_ENABLE_BENCH` 限定の diagnostic state として保持・解放するようにした。Issue本文も diagnostic 境界を反映する形に修正した。
- Fix commit: `pending`
- Verification: `target PHPT PASS; bench build with --enable-grpc-bench PASS and grpc_lite_server_streaming_open exported`
- Notes: request payload ownershipは引き続き `state->request` が担っており、`data_source_read_callback` が参照する `state->call.request` の lifetime は resource destructor まで維持される。request metadata は `append_custom_request_headers()` と `nghttp2_submit_request()` の submit 前だけで使われ、response metadata/status は `grpc_call` 側に保持されるため、`state->metadata` 削除自体には lifecycle reliance は見つからなかった。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
