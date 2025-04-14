fun main(t: int?) {
    t!;
    t! as int;

    t as int;
}

/**
@compilation_should_fail
@stderr type `int?` can not be cast to `int`
@stderr t as int;
 */
