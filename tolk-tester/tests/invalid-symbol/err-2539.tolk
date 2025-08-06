import "@stdlib/tvm-dicts"
import "../imports/use-dicts-err"

/**
@compilation_should_fail
@stderr tests/imports/use-dicts-err.tolk:2:22
@stderr Using a non-imported symbol `createEmptyDict`
@stderr hint: forgot to import "@stdlib/tvm-dicts"?
 */
