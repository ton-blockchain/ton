import "../imports/some-math.tolk";
import "../imports/invalid-no-import.tolk";

/**
@compilation_should_fail
@stderr ../imports/invalid-no-import.tolk:2:13
@stderr Using a non-imported symbol `someAdd`
 */
