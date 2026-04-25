<?php
declare(strict_types=1);

/**
 * Shared helpers for the PoC clients: minimal protobuf wire codec and gRPC
 * length-prefix framing. Replaced by protoc-generated classes / a real
 * extension in later phases.
 */

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

function pb_encode_string_field(int $tag, string $value): string
{
    $key = ($tag << 3) | 2;
    return chr($key) . pb_encode_varint(strlen($value)) . $value;
}

/** field 1 string of HelloReply { string message = 1; } */
function decode_hello_reply(string $payload): string
{
    $pos = 0;
    $key = pb_decode_varint($payload, $pos);
    $tag = $key >> 3;
    $wire = $key & 0x07;
    if ($tag !== 1 || $wire !== 2) {
        throw new RuntimeException("unexpected field: tag=$tag wire=$wire");
    }
    $len = pb_decode_varint($payload, $pos);
    return substr($payload, $pos, $len);
}

/** Wrap payload as a gRPC frame: 1B flag + 4B BE length + payload. */
function grpc_frame(string $payload): string
{
    return "\x00" . pack('N', strlen($payload)) . $payload;
}
