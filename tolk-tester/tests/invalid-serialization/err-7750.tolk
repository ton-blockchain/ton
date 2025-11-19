type CustomInt = int;

fun CustomInt.packToBuilder(self, mutate b: builder) {
}

struct WithC {
    a: int32;
    b: CustomInt;
}

fun main() {
    WithC.fromSlice("");
}

/**
@compilation_should_fail
@stderr auto-serialization via fromSlice() is not available for type `WithC`
@stderr because field `WithC.b` of type `CustomInt` can't be serialized
@stderr because type `CustomInt` defines a custom pack function, but does not define unpack
@stderr hint: declare unpacker like this:
@stderr fun CustomInt.unpackFromSlice(mutate s: slice): CustomInt
 */
