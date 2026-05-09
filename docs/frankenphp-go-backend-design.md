# FrankenPHP grpc-go backend design

## Goal

`php-grpc-lite` keeps the public `Grpc\` surface compatible with the official PHP gRPC package while allowing the transport implementation to be selected at channel construction time.

The global default is `grpc_lite.backend=auto`. In normal PHP runtimes this resolves to the built-in HTTP/2 transport in `ext/grpc`. In FrankenPHP runtimes that load the `FrankenGrpc\*` extension surface, `auto` resolves to the optional grpc-go backend.

## Backend selection

The backend is selected when `Grpc\Channel` is constructed.

Supported values:

| value | behavior |
| --- | --- |
| `auto` | Use `franken-go` when an internal `FrankenGrpc\Channel` class is loaded; otherwise use `http2`. |
| `http2` | Use the built-in nghttp2/OpenSSL/socket transport. |
| `franken-go` | Require `FrankenGrpc\Channel` and delegate calls to it. |

Global default:

```ini
grpc_lite.backend=auto
```

Per-channel override:

```php
$channel = new Grpc\Channel($target, [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc_lite.backend' => 'franken-go',
]);
```

`auto` intentionally requires an internal `FrankenGrpc\Channel` class so that IDE stubs or userland test doubles do not silently change production transport behavior. Explicit `franken-go` remains useful for tests and development.

## Responsibility split

`php-grpc-lite` owns:

- `Grpc\` public API compatibility.
- Official `grpc/grpc` wrapper bridge behavior.
- Request metadata validation, call credential metadata, user-agent, deadline conversion, status object shape, and `startBatch()` semantics.
- Backend selection.

The HTTP/2 backend owns:

- TCP/TLS connection lifecycle.
- nghttp2 session and stream lifecycle.
- gRPC frame parsing/building.
- Persistent connection cache.

The FrankenPHP grpc-go backend owns:

- grpc-go `ClientConn` lifecycle.
- grpc-go unary and server-streaming execution.
- grpc-go metadata/status/deadline mapping at the Go boundary.
- FrankenPHP worker-safe Go resource lifecycle.

## Franken backend contract

The delegated PHP classes are expected under the `FrankenGrpc` namespace:

```php
namespace FrankenGrpc;

final class Channel
{
    public function __construct(string $target, array $options = []);
    public function close(): void;
}

final class UnaryCall
{
    public function __construct(Channel $channel, string $method);

    public function start(
        string $payload,
        array $metadata = [],
        ?float $timeoutSeconds = null,
    ): UnaryResult;
}

final class ServerStreamingCall
{
    public function __construct(Channel $channel, string $method);

    public function start(
        string $payload,
        array $metadata = [],
        ?float $timeoutSeconds = null,
    ): void;

    public function read(): ?string;
    public function getInitialMetadata(): array;
    public function getStatus(): Status;
    public function getTrailingMetadata(): array;
    public function cancel(): void;
    public function getPeer(): string;
}
```

Payloads are serialized protobuf message bytes, not gRPC 5-byte framed messages. Metadata is `array<string, list<string>>`. `timeoutSeconds` is a relative timeout from call start.

## Current limitations

- The first integration target is unary and server streaming only.
- Client streaming and bidirectional streaming remain out of scope.
- TLS/mTLS option mapping depends on the FrankenPHP grpc-go extension contract and must be verified before `auto` is recommended for production TLS workloads.
- Backend fallback is not performed after channel construction. A selected backend failure is reported as that backend's failure.

## Verification

The repository provides a CI-reproducible smoke environment that builds FrankenPHP with `github.com/dkkoma/frankenphp-grpc-go-client` from GitHub, not from a sibling checkout.

```bash
docker compose build dev-franken-grpc-go
docker compose run --rm dev-franken-grpc-go tools/test/check-franken-go-backend.sh
```

The default fetched ref is `main`; override with `FRANKEN_GRPC_GO_CLIENT_REF=<ref>` when needed.
