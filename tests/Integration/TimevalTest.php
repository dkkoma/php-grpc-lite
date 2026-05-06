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

    public function testArithmeticSaturatesInsteadOfOverflowing(): void
    {
        self::assertSame(PHP_INT_MAX, (new Timeval(PHP_INT_MAX))->add(new Timeval(1))->microtime());
        self::assertSame(PHP_INT_MIN, (new Timeval(PHP_INT_MIN))->subtract(new Timeval(1))->microtime());
    }

    public function testCompareDoesNotOverflow(): void
    {
        self::assertSame(1, (new Timeval(PHP_INT_MAX))->compare(new Timeval(PHP_INT_MIN)));
        self::assertSame(-1, (new Timeval(PHP_INT_MIN))->compare(new Timeval(PHP_INT_MAX)));
        self::assertSame(0, (new Timeval(123))->compare(new Timeval(123)));
    }
}
