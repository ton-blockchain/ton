type Same = int32
type Wrapper = int32

fun use(mutate v: Wrapper): void { v = 9; }

fun main(flag: bool) {
    var x: Same | slice = flag ? (1 as Same) : beginCell().endCell().beginParse();
    if (x is Same) {
        use(mutate x);
    }
}

/**
@compilation_should_fail
@stderr can not pass `Same | slice` to mutate `Wrapper`, because mutation is not type compatible
 */
