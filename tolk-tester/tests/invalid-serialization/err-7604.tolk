struct CCC {
    v: int;
}

fun main() {
    var options = CreateMessageOptions {
        bounce: true,
        dest: address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF"),
        value: 0,
        body: CCC { v: 10 }
    };
    createMessage(options);
}

/**
@compilation_should_fail
@stderr error: auto-serialization via createMessage() is not available for type `CCC`
@stderr because field `CCC.v` of type `int` can't be serialized
@stderr because type `int` is not serializable, it doesn't define binary width
@stderr hint: replace `int` with `int32` / `uint64` / `coins` / etc.
 */
