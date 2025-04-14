fun main(a: int | slice) {
    match (a) {
        int => {
            var cc: int = a;
        },
        slice => {
            return cc;
        },
    };
}

/**
@compilation_should_fail
@stderr undefined symbol `cc`
@stderr return cc;
 */
