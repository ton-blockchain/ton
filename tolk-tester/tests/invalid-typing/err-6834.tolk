type SnakedCell<T> = cell

type MInt_v1 = int
type MInt_v2 = int

fun m(a: SnakedCell<MInt_v1>, b: SnakedCell<MInt_v2>) {
    a = b
}

/**
@compilation_should_fail
@stderr can not assign `SnakedCell<MInt_v2>` to variable of type `SnakedCell<MInt_v1>`
@stderr hint: use `as` operator for UNSAFE casting: `<some_expr> as SnakedCell<MInt_v1>`
 */
