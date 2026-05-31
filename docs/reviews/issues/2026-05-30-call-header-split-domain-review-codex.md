# call header split domain review 2026-05-30

## Scope

- `src/call.h` rename/split into `src/grpc_exchange_state.h`, `src/grpc_result.h`, and `src/diagnostic/bench_call.h`
- Adjacent includes in `src/transport.h`, `src/status_core.c`, `src/unary_call.h`, `src/server_streaming_call.h`, `src/wrapper_adapter.h`, `src/wrapper_adapter.c`, and `tests/unit/test_status_core.c`
- Current docs references in `docs/guides/code-reading-guide.md` and `docs/verification/protocol-model-review-guide.md`

## Reviewer Role

- Domain model reviewer

## Review Prompt Summary

- Repository-specific review of the staged header split, focusing on naming, responsibility boundaries, public/internal boundaries, and production vs diagnostic/bench boundaries. Production code was not modified.

## Issues

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none

## Review Notes

- `grpc_exchange_state.h` now names the `grpc_call` owner more precisely as 1 RPC over 1 HTTP/2 stream exchange state. Keeping the C type name unchanged is compatible with the repository's current internal vocabulary and avoids conflating this header move with a semantic type rename.
- `grpc_result.h` contains wrapper/orchestration result DTOs (`grpc_lite_status_result`, `grpc_lite_unary_result`, `grpc_lite_streaming_next_result`) and is included by `transport.h`, where those DTOs are already part of the internal helper contract. I did not see a public C API boundary leak.
- `src/diagnostic/bench_call.h` contains the bench-only observation fields and is only pulled into `grpc_exchange_state.h` under `PHP_GRPC_LITE_ENABLE_BENCH`. The production build still has no diagnostic PHP entrypoint exposure through this split.
- Adjacent wrapper and surface includes continue to separate `Grpc\Call::startBatch()` adapter declarations from diagnostic helpers; `wrapper_adapter.h` no longer exports the diagnostic header transitively.
- At review time, only the `src/call.h` to `src/grpc_exchange_state.h` rename was staged while the split headers and adjacent include updates were unstaged/untracked. This was a staging hygiene note rather than a domain-model finding.

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
