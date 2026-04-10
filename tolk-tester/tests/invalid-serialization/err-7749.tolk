type CustomInt = int;

fun CustomInt.unpackFromSlice(mutate s: slice) {
    return 10;
}

struct WithC {
    a: int32;
    b: CustomInt;
}

fun main(w: WithC) {
    beginCell().storeAny(w);
}

/**
@compilation_should_fail
@stderr auto-serialization via storeAny() is not available for type `WithC`
@stderr because field `WithC.b` of type `CustomInt` can't be serialized
@stderr because type `CustomInt` defines a custom unpack function, but does not define pack
@stderr hint: declare packer like this:
@stderr fun CustomInt.packToBuilder(self, mutate b: builder)
 */
