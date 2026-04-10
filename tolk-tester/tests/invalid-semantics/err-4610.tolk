fun int.create(value: int) {
    return value;
}

fun main() {
    10.create();
}

/**
@compilation_should_fail
@stderr method `int.create` can not be called via dot
@stderr it's a static method, it does not accept `self`
 */
