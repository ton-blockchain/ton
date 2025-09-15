import "../imports/some-helpers"

fun usingNonImported() {
    var s: SomeStorage = {};
}

/**
@compilation_should_fail
@stderr Using a non-imported symbol `SomeStorage`
@stderr forgot to import "some-storage.tolk"?
 */
