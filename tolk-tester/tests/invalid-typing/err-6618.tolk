struct Ok<T> { result: T }
struct Err<T> { errPayload: T }

type Response<TResult, TError> = Ok<TResult> | Err<TError>;

fun f(w: Response<int, slice> | Response<int8, builder>) {
    match (w) {
        Ok<int8> => {}
        Err<slice> => {}
    }
}

/**
@compilation_should_fail
@stderr `match` does not cover all possible types
@stderr missing types are: `Ok<int>`, `Err<builder>`
 */
