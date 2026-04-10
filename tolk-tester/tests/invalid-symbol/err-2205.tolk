import "@two-dots/imports/some-math.tolk";
import "../imports/some-math.tolk";
import "../imports/invalid-no-import.tolk";

/**
@path_mapping @two-dots={DIR}/..
@compilation_should_fail
@stderr tests/imports/invalid-no-import.tolk:2:13
@stderr Using a non-imported symbol `someAdd`
 */
