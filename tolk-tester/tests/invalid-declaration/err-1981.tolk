struct(0x1) Ok1 {}
struct(0x2) Ok2 {}
struct NoPrefix {}

type BadClientBody = Ok1 | NoPrefix | Ok2

struct Envelope {
    @abi.clientType(Cell<BadClientBody>)
    rawBody: cell
}

/**
@compilation_should_fail
@stderr invalid `@abi.clientType`: type `Cell<BadClientBody>` can not be serialized
@stderr because alias `BadClientBody` expands to `Ok1 | NoPrefix | Ok2`
@stderr because could not automatically generate serialization prefixes for a union
@stderr because struct `Ok2` has opcode, but `NoPrefix` does not
 */
