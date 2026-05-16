---
Status: Closed
Owner: Codex
Created: 2026-05-16
Scope: ext/grpc persistent connection key and reuse path
Role: HTTP/2/gRPC domain model review
---

# persistent connection key domain review

## Findings

- Blocker: none
- High: none after fix
- Medium: none after fix
- Low: bench direct arbitrary key issue fixed by internal canonical key derivation

## Reviewed decisions

- Persistent connection identity is represented by `Grpc\Channel` identity, not by re-reading TLS bytes at every RPC.
- The production connection key is `sha256:<64hex>` over length-prefixed canonical identity fields: host, port, authority, TLS verify name, credential type, and SHA-256 digests of root certs / cert chain / private key.
- Persistent entry stores only the canonical key identity. This removes per-RPC PEM revalidation while preserving credential isolation under exact hash key lookup.
- Bench direct APIs also derive canonical keys internally, so diagnostic paths do not regain arbitrary-key aliasing.

## Verification

- TLS/mTLS PHPT: PASS
- full PHPT: PASS, 15/15
- C static analysis: PASS
- real Spanner mixed c16: CPU/request improved from ~11.4ms to ~9.2-9.9ms.

## Follow-up review findings

- High: non-injective delimiter key was fixed by length-prefixed canonical identity + SHA-256 digest.
- Medium: key length divergence was fixed by fixed-length `sha256:<64hex>` key.
- Low: bench direct arbitrary key was fixed by internal canonical key derivation; the old `grpc_lite_channel_close($key)` diagnostic API was removed because caller-supplied key no longer maps to the internal canonical key.

## Final status

- Blocker: none
- High: none
- Medium: none
- Low: none known
