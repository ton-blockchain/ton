struct(0x1) Ok1 {}
struct(0x2) Ok2 {}
struct(0x3) Ok3 {}
struct NoPrefix {}

type MsgOk1 = Ok1 | Ok2 | Ok3;

type MsgCantBe = Ok1 | Ok2 | NoPrefix | Ok3;

fun main() {
    MsgOk1.fromCell(beginCell().endCell());

    MsgCantBe.fromCell(beginCell().endCell());
}

/**
@compilation_should_fail
@stderr auto-serialization via fromCell() is not available for type `MsgCantBe`
@stderr because alias `MsgCantBe` expands to `Ok1 | Ok2 | NoPrefix | Ok3`
@stderr because could not automatically generate serialization prefixes for a union
@stderr because struct `Ok3` has opcode, but `NoPrefix` does not
@stderr hint: manually specify opcodes to all structures
@stderr err-7419.tolk:13
 */
