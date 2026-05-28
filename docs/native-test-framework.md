# Native extension test framework

`ext/grpc` は PHP extension として壊れ方が PHP userland と違うため、テストは目的別の gate に分ける。

## Gate structure

| gate | command | purpose |
|---|---|---|
| C unit | `./tools/test/check-c-unit.sh` | pure C helperの境界値、gRPC status taxonomy、timeout formatなどを高速に検証する |
| PHPT integration | `./tools/test/check-phpt.sh` | PHP extension surface、unary/server streaming、metadata、deadline、TLS/mTLS、resource lifecycleを検証する |
| C fuzz smoke | `./tools/test/check-c-fuzz.sh` | pure C protocol boundaryをlibFuzzer + ASan/UBSanで短時間探索する |
| Sanitizer | `./tools/test/check-c-sanitizer.sh` / `check-c-msan.sh` / `check-c-tsan.sh` | memory safety、UB、thread-safety regressionsをSanitizer PHP build上で検出する |
| ZTS PHPT | `./tools/test/check-zts-phpt.sh` | ZTS PHP build上でextension build/loadとPHPT integrationを検証する |
| ZTS performance comparison | `./tools/test/check-zts-performance.sh` | representative benchmarkをNTS/ZTS同条件で実行し、ZTS固有の性能差をOTEL run idで比較する |
| Valgrind / lifecycle | `./tools/test/check-c-valgrind.sh` / lifecycle scripts | sanitizerでは見落としやすいrelease lifecycle leak、FD/RSS増加、request-boundary reuseを検証する |

## Development gate

通常のC実装変更では次を通す。

```bash
./tools/test/check-native-development-gate.sh
```

このgateは、静的解析、C unit、PHPT、短時間fuzz smokeを実行する。長時間stress、Valgrind、全Sanitizer matrixはrelease gate側に置く。

GitHub Actions の `Native QA` workflow でも push / pull request ごとに同じ development gate を実行する。

ZTSに触る変更、module globals、persistent connection cache、object/resource lifecycleに触る変更では次も通す。

```bash
./tools/test/check-zts-phpt.sh
```

ZTS正式サポートのQAでは、機能gateに加えてNTSとの代表性能比較も取る。

```bash
BENCH_TAG=zts-compare-YYYYMMDD ./tools/test/check-zts-performance.sh
```

default suiteは `spanner-shape metadata-header`。必要に応じて `ZTS_PERF_SUITES` と `ZTS_PERF_ARGS` で対象と呼び出し数を調整する。

## Coverage gate

CIでC coverageを確認する。

```bash
./tools/test/check-c-coverage.sh
```

このgateはgcov/lcov instrumentation付きでC unitとPHPTを実行し、`var/coverage/c-lcov/summary.txt`、`var/coverage/c-lcov/html/`、Codecov upload用の `var/coverage/c-lcov/codecov.info` を生成する。GitHub ActionsではHTML/lcov一式をartifactとして保存し、Codecovには `native-c` flagでuploadする。

## Release / QA gate

release前、またはtransport/lifecycle/resource管理を変える変更では次を通す。

```bash
./tools/test/check-native-release-hardening.sh
```

このgateはdevelopment gate相当の検証に加えて、ASan/UBSan、MSan C core、TSan、Valgrind、lifecycle stress、FPM request-boundary lifecycleを実行する。

ZTS正式サポートのrelease判定では、ZTS PHPTをCI上の必須gateとして扱い、NTS/ZTS代表性能比較をmanual QA evidenceとしてissueまたはbenchmark docsへ記録する。

## Boundary and fuzzing policy

- deterministic boundary testsは `ext/grpc/tests/unit/` に置く。
- libFuzzer harnessは `ext/grpc/tests/fuzz/` に置く。
- seed corpusは `ext/grpc/tests/fuzz/corpus/` にコミットする。runnerは `var/fuzz/corpus/` にコピーして実行し、seed corpusへ探索結果を書き戻さない。
- fuzz smokeはCI/releaseで再現可能な短時間探索にする。crash artifactは `var/fuzz/artifacts/` に出す。
- protocol parserやlength/status/timeoutのようなpure C関数は、PHPTだけに依存せずC unitとfuzz harnessを持つ。

## Sanitizer scope

- ASan/UBSan: Clang-built PHP + extensionでC unitと全PHPTを通す。
- TSan: Clang-built PHP + extensionでC unitと全PHPTを通す。PHPのextension dlopenでTSan非互換な `RTLD_DEEPBIND` を使わないよう、Sanitizer image内のPHP sourceをpatchしてbuildする。
- MSan: Debian配布のOpenSSL/nghttp2などがMSan instrumentationなしのため、transport integrationには使わない。最終gateではpure C core unitに限定する。

## Coverage expectation

PHPT coverageはC extensionのpublic behaviorを守るためのもの。C unit/fuzzは、PHPTで到達しづらい境界値、整数境界、短いbuffer、invalid byte列、status/timeout taxonomyを守るためのもの。どちらか一方で代替しない。
