<?php

declare(strict_types=1);

use BenchApp\Bench\SpannerBench;
use Illuminate\Http\Request;
use Illuminate\Support\Facades\Route;

Route::get('/bench', static function (Request $request, SpannerBench $bench) {
    $action = (string) $request->query('action', 'transaction_select2_update1_insert1');
    $result = match ($action) {
        'select_1row_10col' => $bench->selectOneRow(),
        'dml_insert_10col' => $bench->dmlInsert(),
        'dml_update_10col' => $bench->dmlUpdate(),
        'dml_delete_10col' => $bench->dmlDelete(),
        'transaction_select2_update1_insert1' => $bench->mixedTransaction(),
        default => throw new InvalidArgumentException("unknown action: $action"),
    };

    return response()->json(['ok' => true, 'pid' => getmypid(), 'action' => $action] + $result);
});
