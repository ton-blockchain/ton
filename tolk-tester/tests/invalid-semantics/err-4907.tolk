struct Data<T> {
    data: T;
}

struct Parameters<TBody, TData> {
    bounce: bool,
    dest: slice | Data<TData>,
    body: TBody,
}

fun f<TBody, TData>(params: Parameters<TBody, TData>) {
}

fun main() {
    f({
        dest: "sss",
        body: 123,
    })
}

/**
@compilation_should_fail
@stderr can not deduce TData for generic struct `Parameters<TBody, TData>`
@stderr dest: "sss"
 */
