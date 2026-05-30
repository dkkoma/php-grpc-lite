# PR before/after domain maintainability review 2026-05-30

## Scope

- Branch: `c-practice-work-plan`
- Base: merge-base with `main` at `b5592d4630ed092670aea6882d9369683393d458`
- Main changed areas: repository-root extension layout, C translation units, internal headers, `Grpc\` surface vs wrapper adapter vs transport boundaries, franken-go/backend removal, issue/review documentation.

## Reviewer Role

- Domain/modeling and maintainability reviewer

## Review Prompt Summary

- Review the whole current PR branch against the base branch as a before/after comparison. Focus on source layout, header boundaries, public/internal API boundaries, PHP surface vs wrapper adapter vs transport responsibilities, naming, diagnostic boundaries, and issue/review documentation quality. Do not modify production code.

## Before / After / Benefits / Costs / Recommendation

### Before

- The extension lived under `ext/grpc/` while the repository was already moving toward a repository-root source-build model.
- `main.c` directly included production `.c` files and bench/diagnostic `.c` files, and `transport.c` directly included `transport_core.c`; the issue record captures this explicitly in `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md`.
- `internal.h` was the effective private API for nearly every concept: PHP surface objects, module globals, gRPC call state, HTTP/2 transport state, status/protocol helpers, and bench diagnostics.
- Runtime backend selection and franken-go delegation were still represented in the production surface (`grpc_lite.backend`, `GRPC_LITE_BACKEND_*`, `franken_channel`, `franken_server_streaming_call`) even though the current SPEC and AGENTS model say runtime transport is nghttp2 only.
- The name `bridge.c` described a generic implementation detail rather than the actual domain role: adapting the official `grpc/grpc` wrapper batch API into php-grpc-lite call orchestration.

### After

- `config.m4` at repository root now lists explicit production sources and only appends `src/diagnostic/diagnostic.c` / `src/diagnostic/bench.c` for `--enable-grpc-bench`.
- `grpc.c` is limited to module lifecycle, INI, class registration delegation, constants, and module entry; `Grpc\*` class/object implementation moved to `src/surface.c`.
- The wrapper adapter is named and placed as `src/wrapper_adapter.c`; `Grpc\Call::startBatch()` is the visible adapter entrypoint, and simple `Grpc\Call` lifecycle methods belong to the PHP surface.
- Call/result state was split into `src/grpc_exchange_state.h`, `src/grpc_result.h`, and `src/diagnostic/bench_call.h`, which makes production call exchange, wrapper result DTOs, and bench-only measurement state easier to distinguish.
- `src/` is now consistently C/internal-header source, while PHP support code moved under `support/php/`.
- franken-go/backend selection was removed from current production code and docs, matching the nghttp2-only transport model.

### Benefits

- The PR materially improves maintainability. The build graph now matches the implementation graph, static analysis can inspect real translation units, and C unit/fuzz tests no longer depend on direct `.c` inclusion.
- Responsibility names are clearer: `grpc.c` for the extension module entrypoint, `surface.c` for PHP-visible low-level classes, `wrapper_adapter.c` for official wrapper batch adaptation, `unary_call.c` / `server_streaming_call.c` for call-type orchestration, and `transport.c` for HTTP/2/nghttp2 transport.
- Public vs internal API intent is clearer. No `include/` public C API was added; internal headers live under `src/`, and `src/internal.h` is now a compatibility aggregate rather than the main design boundary.
- The production vs diagnostic boundary is stronger than before because diagnostic entrypoints are built only under `--enable-grpc-bench`, and bench call state is isolated in `src/diagnostic/bench_call.h`.
- Removing backend selection is a domain improvement, not only cleanup: it eliminates a production model contradiction with the current SPEC.
- The work-plan issue is unusually good for later archaeology: it records purpose, non-scope, phase boundaries, risks, validation, commits, review records, and explicit tradeoff decisions.

### Costs or risks

- The PR introduces many files and headers. The code is easier to navigate by domain, but a contributor now has to understand the internal header map and build source list rather than opening one included `main.c`.
- `src/transport.h` is still a broad internal API. It exposes `h2_connection`, `server_streaming_call_state`, connection cache operations, nghttp2 callbacks, socket/TLS helpers, request header builders, response metadata functions, status result builders, and cleanup helpers in one header. This is substantially better than the old all-purpose `internal.h`, but it is not yet a narrow transport boundary.
- `src/common.h` still centralizes PHP/Zend, nghttp2, OpenSSL, socket, and system includes. This is acceptable as a private compatibility convenience, but it keeps compile-time coupling wider than the domain model ideally needs.
- `grpc_call` remains a mixed exchange state for one RPC over one HTTP/2 stream: it includes stream identity, gRPC status/metadata parse state, response queues, request write state, deadline/I/O failure fields, and conditional bench fields. The new filename is clearer, but the type itself is still a large cross-layer state carrier.
- Root `src/` is now C-owned and PHP support moved to `support/php/`. This is a good extension-repo convention, but it is a real package-layout change that future Composer/library work must remember.

### Recommendation

The tradeoff looks worthwhile. The branch removes a real design contradiction, turns implicit unity-build coupling into explicit source/header boundaries, and improves the ability to review and test the C implementation without changing the PHP/gRPC compatibility model.

I do not see any actionable Blocker, High, Medium, or Low finding for this PR. The residual downsides are best treated as accepted design decisions for this branch: keep `transport.c` and `transport.h` broad while behavior is stable, then split connection/callback/request/response helpers under a future transport-specific issue with its own review gate.

## Issues

### REVIEW-20260530-001: No actionable domain/modeling findings

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `Domain/modeling and maintainability reviewer`
- Finding: The before/after tradeoff is favorable and no production-code change is required before merge from a domain/modeling standpoint.
- Evidence: `config.m4:15-19` lists explicit production and bench-only sources; `grpc.c:65-77` delegates PHP class registration and keeps module lifecycle/constants in the entrypoint; `docs/code-reading-guide.md:97-157` documents the current surface/wrapper adapter/transport split; `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:605-630` records final state and verification.
- Expected model: The extension should expose official `Grpc\` low-level surface while keeping Composer wrapper adaptation, call orchestration, HTTP/2 transport, and bench diagnostics in distinguishable internal boundaries.
- Why it matters: This PR is primarily maintainability work. The important question is whether the new boundaries make future protocol and lifecycle reviews easier without fragmenting responsibilities beyond recognition.
- Recommended fix: No required fix. Merge is reasonable if the team accepts the residual broad `transport.h` / large `grpc_call` tradeoff.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: Manual PR diff review against merge-base; no tests executed as part of this review.
- Notes: Existing issue record reports build, PHPT, C unit, static analysis, coverage, fuzz smoke, PHPUnit, and selected benchmark verification.

### REVIEW-20260530-002: Keep broad transport header as an explicit follow-up boundary

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `Domain/modeling and maintainability reviewer`
- Finding: `src/transport.h` is still a large internal API rather than a narrow HTTP/2 transport facade, but the PR documents this as a staged tradeoff and avoids mixing it with behavior changes.
- Evidence: `src/transport.h:23-149` exposes connection structs, server streaming resource state, connection cache lifecycle, nghttp2 callbacks, socket/TLS helpers, request header helpers, response metadata helpers, and status/result conversion helpers; `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:524-534` says transport sub-splitting is intentionally deferred; `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:548-564` records this risk and decision.
- Expected model: Transport internals can remain private and broad during a structural migration, but future narrowing should follow domain objects: connection/cache, callbacks, request headers, response processing, and pure helpers.
- Why it matters: If new features are added through the current broad header, `transport.h` can become the new `internal.h`, only with a better name.
- Recommended fix: No pre-merge fix. Open a separate transport-boundary issue before further HTTP/2 lifecycle work that splits `src/transport.h` by domain object rather than by convenience.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: Manual header and issue review.
- Notes: This is not a blocker because the PR already improves the previous state and keeps the broad API private.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `2`
