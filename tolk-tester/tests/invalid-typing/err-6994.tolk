type Same = int32
type Wrapper = int32

fun use(mutate v: Wrapper): void { v = 9; }

fun main() {
    var x: Same = 1;
    use(mutate x);
}

/**
@compilation_should_fail
@stderr can not pass `Same` to mutate `Wrapper`, because mutation is not type compatible
 */
