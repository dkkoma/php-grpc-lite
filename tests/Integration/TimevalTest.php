<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\Timeval;
use PHPUnit\Framework\TestCase;

final class TimevalTest extends TestCase
{
    public function testMicrotimeReturnsStoredMicroseconds(): void
    {
        self::assertSame(123456, (new Timeval(123456))->microtime());
    }
}
