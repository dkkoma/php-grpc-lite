# Native extension test framework

repository rootの `grpc` extension は PHP extension として壊れ方が PHP userland と違うため、テストは目的別の gate に分ける。

## Gate structure

| gate | command | purpose |
|---|---|---|
| C unit | `./tools/test/check-c-unit.sh` | pure C helperのboundary value、gRPC status taxonomy、timeout formatなどを高速に検証する |
| PHPT integration | `./tools/test/check-phpt.sh` | PHP extension surface、unary/server streaming、metadata、deadline、TLS/mTLS、resource lifecycleを検証する |
| C fuzz smoke | `./tools/test/check-c-fuzz.sh` | pure C protocol boundaryをlibFuzzer + ASan/UBSanで短時間探索する |
| Sanitizer | `./tools/test/check-c-sanitizer.sh` / `check-c-msan.sh` / `check-c-tsan.sh` | memory safety、UB、thread-safety regressionsをSanitizer PHP build上で検出する |
| ZTS PHPT | `./tools/test/check-zts-phpt.sh` | ZTS PHP build上でextension build/loadとPHPT integrationを検証する |
| ZTS performance comparison | `./tools/test/check-zts-performance.sh` | representative benchmarkをNTS/ZTS同条件で実行し、ZTS固有の性能差をOTEL run idで比較する |
| ZTS parallel call path | `./tools/test/check-zts-parallel-performance.sh` | NTS multi-processとZTS threadでPHP userlandからunary/server streaming call pathを並列実行し、thread並列時のthroughput/latencyを比較する |
| Valgrind / lifecycle | `./tools/test/check-c-valgrind.sh` / lifecycle scripts | sanitizerでは見落としやすいrelease lifecycle leak、FD/RSS増加、request-boundary reuseを検証する |
| PHPUnit integration | `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` | official wrapper / fixture client / Spanner emulatorを含む広い互換性を検証する。現時点ではlocal/release compatibility suiteであり、Native QA必須jobではない |
| slow-consumer比較 | `./tools/test/check-native-slow-consumer.sh` | slow consumer時の観測用比較。現時点ではthreshold付きgateではなくmeasurement-only |

## Development gate

通常のC実装変更では次を通す。

```bash
./tools/test/check-native-development-gate.sh
```

このgateは、静的解析、C unit、PHPT、短時間fuzz smokeを実行する。長時間stress、Valgrind、全Sanitizer matrixはrelease gate側に置く。

GitHub Actions の `Native QA` workflow でも push / pull request ごとに同じ development gate を実行する。

PHPUnit integrationはPHPTより広い互換性確認であり、`Grpc\BaseStub` / official wrapper / generated-client相当fixture / Spanner emulator pathまで含む。CI必須gateへ入れる場合は、fast non-Spanner groupとSpanner emulator groupを分け、実行時間とemulator state resetの扱いを先に決める。現時点ではlocalまたはrelease判定時に明示実行するsuiteとして扱う。

ZTSに触る変更、module globals、persistent connection cache、object/resource lifecycleに触る変更では次も通す。

```bash
./tools/test/check-zts-phpt.sh
```

ZTS正式サポートのQAでは、機能gateに加えてNTSとの代表性能比較も取る。

```bash
BENCH_TAG=zts-compare-YYYYMMDD ./tools/test/check-zts-performance.sh
```

default suiteは `spanner-shape metadata-header`。必要に応じて `ZTS_PERF_SUITES` と `ZTS_PERF_ARGS` で対象と呼び出し数を調整する。

ZTS thread並列のQAでは次も実行する。

```bash
./tools/test/check-zts-parallel-performance.sh
```

default条件は worker数 `1,2,8`、server側delay `10ms`、unary と server streaming の両方。server streamingは各thread/process workerがPHP userlandの `GreeterClient->BenchServerStream(...)->responses()` を最後まで順次drainする。

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

Lifecycle stressはrelease-hardening gateの一部として、scenario failure、Valgrind leak/error、request-boundary reuse failureを検出する。FD/RSSは現時点では観測値として記録し、比例増加の判定や厳密な閾値追加は環境依存を評価してから別issueで行う。

`check-native-slow-consumer.sh` はrelease-hardening gateに含めず、measurement-onlyの比較入口として扱う。gate化する場合は、RSS/FD/latencyなどの閾値と実行環境をissueに記録してからrunnerへ組み込む。

## Boundary and fuzzing policy

- deterministic boundary testsは `tests/unit/` に置く。
- libFuzzer harnessは `tests/fuzz/` に置く。
- seed corpusは `tests/fuzz/corpus/` にコミットする。runnerは `var/fuzz/corpus/` にコピーして実行し、seed corpusへ探索結果を書き戻さない。
- fuzz smokeはCI/releaseで再現可能な短時間探索にする。crash artifactは `var/fuzz/artifacts/` に出す。
- protocol parserやlength/status/timeoutのようなpure C関数は、PHPTだけに依存せずC unitとfuzz harnessを持つ。

## Sanitizer scope

- ASan/UBSan: Clang-built PHP + extensionでC unitと全PHPTを通す。
- TSan: Clang-built PHP + extensionでC unitと全PHPTを通す。PHPのextension dlopenでTSan非互換な `RTLD_DEEPBIND` を使わないよう、Sanitizer image内のPHP sourceをpatchしてbuildする。
- MSan: Debian配布のOpenSSL/nghttp2などがMSan instrumentationなしのため、transport integrationには使わない。最終gateではpure C core unitに限定する。

## Coverage expectation

PHPT coverageはC extensionのpublic behaviorを守るためのもの。C unit/fuzzは、PHPTで到達しづらいboundary value、integer boundary、短いbuffer、invalid byte列、status/timeout taxonomyを守るためのもの。どちらか一方で代替しない。
