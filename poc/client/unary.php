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

require __DIR__ . '/lib.php';

const HOST = 'test-server';
const PORT = 50051;
const SERVICE_METHOD = '/helloworld.Greeter/SayHello';

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

if (strlen($body) < 5) {
    fwrite(STDERR, "response too short to contain a frame\n");
    exit(1);
}
$flag = ord($body[0]);
$len  = unpack('N', substr($body, 1, 4))[1];
$reply = substr($body, 5, $len);
echo "=== unframed payload ===\n";
echo bin2hex($reply) . "\n";

echo "=== decoded HelloReply ===\n";
echo "  message: " . decode_hello_reply($reply) . "\n";

$grpcStatus = (int)($headers['grpc-status'][0] ?? -1);
$grpcMessage = $headers['grpc-message'][0] ?? '';
echo "=== gRPC status ===\n";
echo "  code: $grpcStatus\n";
echo "  message: $grpcMessage\n";

exit($grpcStatus === 0 ? 0 : 1);
