# protocol classification RST helper domain review 2026-05-31

## Scope

- `src/transport.c`
- `src/transport.h`
- `src/server_streaming_call.c`
- `docs/design/protocol-classification-boundary.md`
- `docs/issues/closed/2026-05-31-protocol-classification-runtime-boundary-refactor.md`

## Reviewer Role

- HTTP/2/gRPC domain model reviewer

## Review Prompt Summary

- `codex/protocol-classification-runtime-boundary-refactor` ブランチを対象に、`nghttp2_submit_rst_stream()` の直接呼び出しを `grpc_lite_submit_rst_stream_if_open()` へ集約する変更を確認した。classification flagsとtransport action、connection/stream/call ownership、metadata/status/deadline semantics、RST_STREAM/GOAWAY/EOF lifecycle、public/internal boundaryを確認観点にした。

## Issues

- 指摘なし。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none

## Verification

- scoped files内に残る `nghttp2_submit_rst_stream()` 呼び出しが `grpc_lite_submit_rst_stream_if_open()` に集約されていることを確認した。
- `grpc_lite_submit_rst_stream_if_open()` はtransport actionのguardと `nghttp2_submit_rst_stream()` submitだけを担当し、classification flags設定、gRPC status解決、connection dead/draining化、cache detach、PHP-visible details生成を行っていないことを確認した。
- invalid status、invalid content type、unsupported compression、malformed frames、message limits、metadata limits、read-ahead limitsのclassification flagsは、従来どおりcall/protocol path側で設定されていることを確認した。
- server streaming cancel/destructor pathはresource completionとpending frame flushを引き続き所有し、connection death/cache detachはsend failureまたはunusable connection stateに結びついたままであることを確認した。
- GOAWAYとinbound RST_STREAM handlingは、outbound stream-local cancellationとは別のinbound lifecycleとして扱われていることを確認した。
- `transport.h` はこのcodebase内のinternal headerであり、helperの宣言追加がPHP API surfaceを増やしていないことを確認した。
- `git diff --check`: pass.
