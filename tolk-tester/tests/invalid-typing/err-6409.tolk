struct Wrapper<T> { value: T }

fun f(w: Wrapper<int> | Wrapper<slice>) {
    w = Wrapper { value: beginCell() };
}

/**
@compilation_should_fail
@stderr can not assign `Wrapper<builder>` to variable of type `Wrapper<int> | Wrapper<slice>`
 */
