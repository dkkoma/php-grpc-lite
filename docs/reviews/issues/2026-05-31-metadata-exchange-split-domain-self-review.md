# Metadata exchange split domain self review 2026-05-31

## Scope

- `src/h2_request_headers.h`
- `src/h2_request_headers.c`
- `src/request_metadata.h`
- `src/request_metadata.c`
- `src/response_metadata.h`
- `src/response_metadata.c`
- `src/metadata_key.h`
- `src/transport.c`
- `src/transport.h`
- `config.m4`
- `tools/test/check-c-static-analysis.sh`
- `docs/issues/open/2026-05-31-php-zend-include-boundary-metadata-exchange-split.md`
- `docs/guides/code-reading-guide.md`

## Reviewer Role

- Domain model reviewer

## Review Prompt Summary

- HTTP/2 / gRPC metadata boundary splitが、request metadata conversion、HTTP/2 header storage、response metadata list、PHP result map conversion、transport lifecycleの責務を正しく分けているかを確認した。

## Issues

### REVIEW-20260531-001: none

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `Domain model reviewer`
- Finding: Blocker / High / Medium / Low の指摘はない。
- Evidence: `src/h2_request_headers.c`, `src/request_metadata.c`, `src/response_metadata.c`, `src/transport.c`
- Expected model: request metadata `zval` conversion、HTTP/2 header storage、response metadata list、PHP array conversion、socket/TLS/nghttp2 transport lifecycleがそれぞれ明確なownerを持つ。
- Why it matters: metadataはgRPC compatibilityとhot pathの両方に関わるため、storage owner、conversion owner、transport controlを混ぜると、教材としての読みやすさと変更時の安全性が落ちる。
- Recommended fix: なし。
- Fix summary: `request_metadata.c` はuser metadata validation/base64 encodingを担当し、`h2_request_headers.c` はowned header storageを担当する。`response_metadata.c` はresponse metadata listとPHP map conversionを担当する。`transport.c` はconnection lifecycle、nghttp2 callback、DATA parser中心に戻した。
- Fix commit: pending
- Verification: `git diff --check`, normal build, bench build, `check-c-static-analysis.sh`, `check-c-unit.sh`, `check-phpt.sh`, before/after `metadata-header`, `spanner-shape`, after `cpu-micro`
- Notes: `h2_request_headers` と response metadata listは `zend_string` ownerなのでPHP/Zend-awareなexchange stateであり、pure C化しない判断が正しい。`grpc_call` sub-struct化はfield layoutに関わるため、このissueでは採用しない判断が妥当。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `1 accepted`
