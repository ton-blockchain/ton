type SnakedCell<T> = cell
type RopedCell<T> = cell

type MInt_v1 = int
type MInt_v2 = int

fun SnakedCell<MInt_v1>.validate(self) {}

fun m(b: RopedCell<MInt_v2>) {
    b.validate()
}

/**
@compilation_should_fail
@stderr method `validate` not found for type `RopedCell<MInt_v2>`
@stderr but it exists for type `SnakedCell<MInt_v1>`
 */
