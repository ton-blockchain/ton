struct Wrapper<T> {
    value: T;
}

type ErrWrapper = int | Wrapper;

/**
@compilation_should_fail
@stderr type `Wrapper<T>` is generic, you should provide type arguments
@stderr int | Wrapper;
 */
