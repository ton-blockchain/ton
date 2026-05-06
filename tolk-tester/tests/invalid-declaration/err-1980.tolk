struct Some<T> {
    value: T
}

struct Wrapper<T> {
    @abi.clientType(string)
    payload: RemainingBitsAndRefs

    @abi.clientType(string)
    badPayload: Some<T>
}

/**
@compilation_should_fail
@stderr `@abi.clientType` is not allowed for fields with generics
 */
