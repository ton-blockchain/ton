struct Point {
    x: int
}

struct Box<T> {
    item: T
    extra: Point = { x: 0 }
}

struct Holder {
    bad: int = Box { item: 1 }
}

/**
@compilation_should_fail
@stderr can not assign `Box<int>` to `int`
 */
