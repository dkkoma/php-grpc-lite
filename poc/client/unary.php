<?php
declare(strict_types=1);

/**
 * PoC: gRPC unary call using raw libcurl + HTTP/2 prior knowledge.
 *
 * Hand-encodes HelloRequest and hand-decodes HelloReply to validate the wire
 * format end-to-end. protoc-generated message classes will replace the manual
 * encoding in the next iteration.
 *
 * Usage (from inside the dev container):
 *   php poc/client/unary.php [name]
 */

const HOST = 'test-server';
const PORT = 50051;
const SERVICE_METHOD = '/helloworld.Greeter/SayHello';

/** Encode a single proto3 length-delimited field (wire type 2). */
function pb_encode_string_field(int $tag, string $value): string
{
    $key = ($tag << 3) | 2;
    return chr($key) . pb_encode_varint(strlen($value)) . $value;
}

function pb_encode_varint(int $n): string
{
    $bytes = '';
    while ($n > 0x7f) {
        $bytes .= chr(($n & 0x7f) | 0x80);
        $n >>= 7;
    }
    $bytes .= chr($n & 0x7f);
    return $bytes;
}

function pb_decode_varint(string $buf, int &$pos): int
{
    $n = 0;
    $shift = 0;
    while (true) {
        $b = ord($buf[$pos++]);
        $n |= ($b & 0x7f) << $shift;
        if (($b & 0x80) === 0) {
            return $n;
        }
        $shift += 7;
    }
}

/** Wrap a payload in a gRPC frame: 1-byte flag + 4-byte BE length + payload. */
function grpc_frame(string $payload): string
{
    return "\x00" . pack('N', strlen($payload)) . $payload;
}

/** Parse a single gRPC frame from $buf starting at $pos. Returns the payload, advances $pos. */
function grpc_unframe(string $buf, int &$pos): string
{
    $flag = ord($buf[$pos]);
    $len = unpack('N', substr($buf, $pos + 1, 4))[1];
    $pos += 5;
    if ($flag !== 0) {
        throw new RuntimeException("compressed frames are not supported in this PoC");
    }
    $payload = substr($buf, $pos, $len);
    $pos += $len;
    return $payload;
}

// --- Build request -----------------------------------------------------------
$name = $argv[1] ?? 'World';
$helloRequest = pb_encode_string_field(1, $name);  // HelloRequest { name = ... }
$frame = grpc_frame($helloRequest);

// --- Send via libcurl --------------------------------------------------------
$headers = [];
$body = '';

$ch = curl_init();
curl_setopt_array($ch, [
    CURLOPT_URL            => sprintf('http://%s:%d%s', HOST, PORT, SERVICE_METHOD),
    CURLOPT_HTTP_VERSION   => CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE,
    CURLOPT_POST           => true,
    CURLOPT_POSTFIELDS     => $frame,
    CURLOPT_HTTPHEADER     => [
        'Content-Type: application/grpc',
        'TE: trailers',
        'User-Agent: php-grpc-lite-poc/0.0.1',
    ],
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_HEADERFUNCTION => function ($ch, string $line) use (&$headers): int {
        $trim = rtrim($line, "\r\n");
        if ($trim !== '' && !str_starts_with($trim, 'HTTP/')) {
            [$k, $v] = array_pad(explode(':', $trim, 2), 2, '');
            $headers[strtolower(trim($k))][] = ltrim($v);
        }
        return strlen($line);
    },
    CURLOPT_WRITEFUNCTION  => function ($ch, string $chunk) use (&$body): int {
        $body .= $chunk;
        return strlen($chunk);
    },
]);

$ok = curl_exec($ch);
$err = curl_error($ch);
$httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);

if ($ok === false) {
    fwrite(STDERR, "curl error: $err\n");
    exit(1);
}

// --- Parse response ----------------------------------------------------------
echo "=== HTTP status: $httpCode ===\n";
echo "=== response headers + trailers ===\n";
foreach ($headers as $k => $vals) {
    foreach ($vals as $v) {
        echo "  $k: $v\n";
    }
}

echo "=== response body (raw) ===\n";
echo bin2hex($body) . "\n";

$pos = 0;
$reply = grpc_unframe($body, $pos);
echo "=== unframed payload ===\n";
echo bin2hex($reply) . "\n";

// HelloReply { message = string at field 1 }
$rpos = 0;
$key = pb_decode_varint($reply, $rpos);
$tag = $key >> 3;
$wire = $key & 0x07;
if ($tag !== 1 || $wire !== 2) {
    fwrite(STDERR, "unexpected proto field: tag=$tag wire=$wire\n");
    exit(1);
}
$len = pb_decode_varint($reply, $rpos);
$message = substr($reply, $rpos, $len);

echo "=== decoded HelloReply ===\n";
echo "  message: $message\n";

$grpcStatus = (int)($headers['grpc-status'][0] ?? -1);
$grpcMessage = $headers['grpc-message'][0] ?? '';
echo "=== gRPC status ===\n";
echo "  code: $grpcStatus\n";
echo "  message: $grpcMessage\n";

exit($grpcStatus === 0 ? 0 : 1);
