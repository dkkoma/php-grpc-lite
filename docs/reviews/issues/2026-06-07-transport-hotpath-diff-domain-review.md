# transport.c hotpath diff domain model review 2026-06-07

## Scope

- `src/transport.c`
- Diff: `main...HEAD`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- Review only the `src/transport.c` diff from the hotpath early-return / inline-cost trial. Focus on repository-specific HTTP/2/gRPC concepts, naming, responsibility boundaries, lifecycle/state transitions, invariants, error taxonomy, and production vs diagnostic boundaries.

## Issues

### REVIEW-20260607-001: response header helper collapses distinct protocol concepts behind a generic state name

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `apply_response_header_state()` does not model one clear domain transition. It mixes gRPC trailer/status semantics, HTTP response validation state, unsupported compression handling, metadata trailing classification, and response body discard side effects under the generic name "state". The caller computes `trailing` before calling it, then passes `&trailing` so the helper can mutate the metadata classification as a side effect. This makes the order of "classify header block", "apply special header semantics", and "persist metadata entry" less explicit than the domain model requires.
- Evidence: `src/transport.c:1882` `response_header_initially_trailing()`, `src/transport.c:1890` `apply_response_header_state()`, `src/transport.c:1963`-`1965` `on_header_callback()`.
- Expected model: HTTP/2 response HEADERS classification, gRPC status/trailer parsing, HTTP content-type validation, grpc-encoding policy, and metadata persistence are related but distinct protocol concepts. The transport callback may orchestrate them, but helper boundaries and names should preserve those concepts rather than collapse them into a generic state applier with an out-param classification mutation.
- Why it matters: This area encodes trailers-only responses, initial metadata, trailing metadata, invalid content-type, unsupported compression, duplicate/invalid grpc-status, and body discard behavior. Hiding the classification mutation inside a broad helper makes future changes likely to put a new header into the wrong phase or to accidentally alter whether a header lands in initial or trailing metadata.
- Recommended fix: Rewrite or revert this split. If keeping helpers, use domain-specific helpers with explicit inputs/outputs, for example one helper that classifies the metadata phase and another that applies library-owned response header semantics. Avoid mutating `trailing` through a generic side-effect helper; return the classification or keep the phase decision visibly in `on_header_callback()`.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `Review only. No fix verified yet.`
- Notes: `No runtime regression was identified in this review; the finding is about domain modeling and maintainability of the protocol state transition.`

### REVIEW-20260607-002: unchecked request header append makes the request metadata builder invariant implicit

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `append_request_header_unchecked()` introduces a second append path where the capacity/lifetime invariant is only implied by the caller. In `append_custom_request_header_value()`, capacity is checked before copying the key/value strings, then the unchecked append is called after ownership registration. The current callsite is probably valid, but the unchecked helper name describes an optimization detail rather than the request metadata builder domain invariant.
- Evidence: `src/transport.c:2206` `append_request_header_unchecked()`, `src/transport.c:2370`-`2410` `append_custom_request_header_value()`.
- Expected model: `h2_request_headers` owns both nghttp2 header entries and the zend strings backing custom metadata values. The builder should make the invariant "there is capacity for nva/name/value ownership before append" explicit at the boundary where custom metadata is accepted.
- Why it matters: Request metadata handling is a public compatibility boundary for generated clients and GAX. If a future callsite uses the unchecked helper without the exact capacity and ownership preconditions, the error mode becomes silent header loss or memory corruption rather than a metadata limit error. Even when correct, the unchecked helper weakens the readability claim because the domain condition is not encoded at the helper boundary.
- Recommended fix: Prefer the checked `append_request_header()` unless the measured benefit is compelling. If the unchecked path remains, rename it to communicate the domain precondition, for example `append_prechecked_request_header()`, and add a local assertion or very small wrapper at the custom metadata callsite that makes the capacity/ownership precheck visible.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `Review only. No fix verified yet.`
- Notes: `This is not a current behavioral bug; it is an invariant clarity issue introduced by the optimization split.`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `1`
- Low: `1`
- Design Decision: `none`
