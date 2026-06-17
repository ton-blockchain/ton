type AliasToInt = int
type AliasToString = string

type AIntOrString = AliasToInt | AliasToString

struct D {
    f: AIntOrString | int
}

/**
@compilation_should_fail
@stderr union variants `AliasToInt` and `int` have the same runtime representation
 */
