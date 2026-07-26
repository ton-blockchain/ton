
fun int.packToBuilder(self, mutate b: builder) {
}

/**
@compilation_should_fail
@stderr this method can not be declared for type `int`
@stderr hint: custom pack/unpack can be declared only for type aliases and structures
 */
