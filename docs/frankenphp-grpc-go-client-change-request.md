# Change request for frankenphp-grpc-go-client

## Purpose

`php-grpc-lite` can now select a `franken-go` backend from `Grpc\Channel`. The backend is delegated to `FrankenGrpc\*` classes exposed by `github.com/dkkoma/frankenphp-grpc-go-client`.

This document is the requested integration contract for that repository.

## Required public surface

Keep the PHP namespace:

```php
namespace FrankenGrpc;
```

Required classes:

- `Channel`
- `UnaryCall`
- `ServerStreamingCall`
- `UnaryResult`
- `Status`

The existing prototype surface is already close to the required shape.

## Channel options

`php-grpc-lite` passes the original `Grpc\Channel` option array to `FrankenGrpc\Channel`.

The Go extension should tolerate unknown options and implement at least:

| option | expected behavior |
| --- | --- |
| `credentials` | Accept the `Grpc\ChannelCredentials` object when present. It may be ignored initially for plaintext-only smoke, but must not crash. |
| `grpc.default_authority` | Use as authority / host override when supported. |
| `grpc.ssl_target_name_override` | Use as TLS server name override when supported. |
| `grpc.primary_user_agent` | Optional; php-grpc-lite already adds `user-agent` metadata before delegation. |
| `grpc.max_receive_message_length` | Enforce or document unsupported. |
| `grpc.max_metadata_size` / `grpc.absolute_max_metadata_size` | Enforce or document unsupported. |

For the first smoke path, plaintext/insecure transport is enough. TLS and mTLS should be added before recommending `grpc_lite.backend=auto` for production TLS workloads.

## Call contract

`UnaryCall::start()` receives:

- serialized protobuf request bytes
- normalized request metadata as `array<string, list<string>>`
- relative timeout seconds or `null`

It returns `UnaryResult`:

- `payload`: serialized protobuf response bytes
- `status`: canonical gRPC `Status`
- `initialMetadata`: response headers
- `trailingMetadata`: response trailers

`ServerStreamingCall::start()` receives the same request shape. `read()` returns one serialized protobuf response payload per call, then `null`. After `null`, `getStatus()` and `getTrailingMetadata()` must be stable and idempotent.

## Metadata and status

- Metadata keys should be lowercase.
- Multiple values for the same key must preserve order.
- `*-bin` metadata values must preserve raw bytes.
- `grpc-status-details-bin` should be returned in trailing/status metadata when grpc-go exposes it.
- Transport failures before a gRPC status exists should throw an exception.
- Application gRPC statuses should be returned as `Status`.

## Lifecycle

- `Channel::close()` must be idempotent.
- Calls started after close should throw a predictable exception.
- `ServerStreamingCall::cancel()` should cancel the grpc-go context.
- PHP object destruction must release Go handles and must not leak goroutines.

## Verification expected before php-grpc-lite enables production guidance

- Unary success and non-OK status.
- Server streaming success and non-OK final status.
- Initial/trailing metadata split.
- Binary metadata round trip.
- Deadline exceeded.
- Cancellation.
- Channel close idempotency.
- Spanner emulator smoke for small unary and small server streaming workload.
