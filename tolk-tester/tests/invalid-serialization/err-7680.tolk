fun main() {
    val m = createMapFromLowLevelDict<int32, builder>(null);
    m.mustGet(1);
}

/**
@compilation_should_fail
@stderr invalid `map`: type `builder` can not be used as a value
 */
