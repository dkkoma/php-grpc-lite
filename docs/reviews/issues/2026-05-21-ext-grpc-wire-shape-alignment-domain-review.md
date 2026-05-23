# ext-grpc wire shape alignment domain review 2026-05-21

## Scope

- `docs/issues/open/2026-05-21-ext-grpc-wire-shape-alignment.md`
- `docs/issues/open/2026-05-21-http2-control-lifecycle-experiments.md`
- `ext/grpc/transport.c`
- `ext/grpc/main.c`
- `ext/grpc/internal.h`
- `ext/grpc/tests/002-ini.phpt`

## Reviewer Role

- HTTP/2/gRPC domain model reviewer

## Review Prompt Summary

- ext-grpc 1.58 SA JSON wire-shape alignment experimentについて、diagnostic profileがdefault-offかつproduction behaviorから分離されているかを確認した。
- 実験conceptとして、ext-grpc observed settings profile、peer `SETTINGS` before first stream、response `DATA` connection `WINDOW_UPDATE`、authorization no-index、HPACK dynamic table tuning、first server `PING`後のone client-origin `PING`を確認した。

## Issues

### REVIEW-20260521-WIRE-001: one-shot client PING is not scoped to post-CreateSession lifecycle

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model reviewer`
- Finding: wire profileのclient-origin `PING` は「CreateSession後、最初のserver PING受信時」とdocsで定義されているが、実装はconnection上の最初のserver-origin `PING`で即座に送信する。`wait_initial_settings_handshake()` 中や最初のstream開始前にserver `PING`が来た場合も `ext_grpc_158_initial_ping_submitted` が立つ。
- Evidence: `docs/issues/open/2026-05-21-ext-grpc-wire-shape-alignment.md` progress / frame-shape sections; `ext/grpc/transport.c:on_frame_recv_callback` lines 2286-2293; `ext/grpc/internal.h:struct _h2_connection` line 476
- Expected model: この実験のdomain eventは単なるconnection-level first server `PING` ではなく、ext-grpc SA JSON traceで観測した「CreateSession後のserver `PING`に対するwire-shape alignment」である。connection control stateとRPC progression markerは別conceptとして扱う必要がある。
- Why it matters: default-off捨て実験なのでproduction riskは低い。ただし、server/proxyがstream開始前にPINGを送る環境では、profileが意図より早くclient-origin `PING`を消費し、CreateSession後のwire shape比較がずれる。trace解釈で「ext-grpcのCreateSession後PING lifecycleを再現した」と誤読される。
- Recommended fix: 実装を残すならdocsを「connection上の最初のserver-origin PING後に1回client-origin PING」と明記する。CreateSession後に寄せるなら、CreateSession stream close / first successful CreateSession completionなどのconnection-owned diagnostic markerを立て、その後のserver-origin `PING`だけでsubmitする。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `review only`
- Notes: `grpc_lite.http2_experimental_ext_grpc_158_wire_profile` はdefault offの `PHP_INI_SYSTEM` booleanであり、この指摘はproduction-readiness blockerではない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `1`
- Design Decision: `none`

## Additional Checks

- Default behavior: no accidental default behavior change found. New behavior is gated by `grpc_lite.http2_experimental_ext_grpc_158_wire_profile=0` by default.
- Production/diagnostic boundary: acceptable. The issue explicitly treats the profile as SA JSON-only, throwaway diagnostic alignment rather than production default candidate.
- ext-grpc observed settings profile: acceptable. The composite flag reuses `ext_grpc_158` naming and observed constants rather than presenting them as HTTP/2 official semantics.
- Peer SETTINGS before first stream: acceptable. Wire profile waits for peer `SETTINGS` and flushes the client `SETTINGS ACK` before first stream, without waiting for the peer ACK to client initial `SETTINGS`, matching the new issue.
- Response DATA connection WINDOW_UPDATE: acceptable as diagnostic. It remains modeled as DATA-chunk-observed connection-level `WINDOW_UPDATE`, consistent with the prior control-lifecycle review caveat.
- Authorization no-index: acceptable. The `NGHTTP2_NV_FLAG_NO_INDEX` behavior is scoped to the wire profile and to lowercase `authorization` metadata.
- HPACK dynamic table tuning: acceptable as diagnostic. The nghttp2 deflate dynamic table size option is only applied to the wire profile session creation path.
