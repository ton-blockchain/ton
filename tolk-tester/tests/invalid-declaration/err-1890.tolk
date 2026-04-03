struct AA {
    forwardPayload: ForwardPayloadInRef
}

struct ForwardPayloadInRef<T> {
    contents: Cell<T>
}

/**
@compilation_should_fail
@stderr type `ForwardPayloadInRef<T>` is generic, you should provide type arguments
 */
