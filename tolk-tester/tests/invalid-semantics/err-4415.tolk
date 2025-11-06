struct Wrap<T> { private item: T }

fun m(w: Wrap<int32>) {
    w = w;
    return w.item!;
}

/**
@compilation_should_fail
@stderr field `Wrap<int32>.item` is private
 */
