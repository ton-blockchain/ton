import "@stdlib/tvm-dicts"
import "../imports/use-dicts-err.tolk"

/**
@compilation_should_fail
@stderr ../imports/use-dicts-err.tolk:2:22
@stderr Using a non-imported symbol `createEmptyDict`
@stderr Forgot to import "@stdlib/tvm-dicts"?
 */
