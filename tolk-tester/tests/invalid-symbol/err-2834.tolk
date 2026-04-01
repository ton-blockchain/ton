import "@stdlib/reflection"

struct (0x00) Base {}

type Fancy = Base

fun Fancy.packToBuilder(self, mutate b: builder) {
    b.storeUint(0, 32);
}

fun main() {
    return reflect.serializationPrefixOf<Fancy>();
}

/**
@compilation_should_fail
@stderr type `Fancy` has a custom serializer
 */
