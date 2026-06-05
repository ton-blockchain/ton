// @compilation_should_fail

// @stderr can not convert `string` to `int33`
// @stderr can not convert `builder` to `int33`
// @stderr can not convert `string` to `int34`
// @stderr can not convert `builder` to `int34`
fun invalidLispList() {
    var m: lisp_list<int33> = ["", beginCell()];
    lisp_list<int34> ["", beginCell()];
}

// @stderr can not convert `builder` to `int35`
// @stderr can not convert `int32` to `int35`
// @stderr can not convert `builder` to `int36`
// @stderr can not convert `int32` to `int36`
// @stderr can not convert `int` to `array<array<unknown>>`
// @stderr can not convert `int32` to `array<tuple>`
fun invalidArray() {
    var a: array<int35> = [1, beginCell(), 4 as int32];
    array<int36> [1, beginCell(), 4 as int32];

    array<array<array<unknown>>> [8];
    array<array<tuple>> [8 as int32];
}

// @stderr can not convert `bool` to `int40`
// @stderr can not convert `builder` to `int41`
// @stderr  invalid `[...]` constructor for `map`: each item must be `[key, value]`
fun invalidMap() {
    var m: map<int40, int41> = [ [true, beginCell()] ];

    var sh: [int42, int43] = [1,2];
    map<int42, int43> [ sh ];
}

// @stderr can not convert `any_address` to `int32`
// @stderr can not convert `string` to `bool`
// @stderr `[...]` for `map` is not supported yet, only `[]` is allowed (empty map)
struct InvalidMapDef {
    m: map<int32, bool> = [
        [createAddressNone(), ""]
    ]
}

