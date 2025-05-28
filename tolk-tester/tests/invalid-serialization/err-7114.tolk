type NotSerializableTensor = (int8, slice);

struct Demo {
    a: int8;
    b: int8 | int16;
    c: NotSerializableTensor;
}

fun main(p: Demo?) {
    p.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `Demo?`
@stderr because field `Demo.c` of type `NotSerializableTensor` can't be serialized
@stderr because alias `NotSerializableTensor` expands to `(int8, slice)`
@stderr because element `tensor.1` of type `slice` can't be serialized
@stderr because type `slice` is not serializable, it doesn't define binary width
@stderr hint: replace `slice` with `address` if it's an address, actually
@stderr hint: replace `slice` with `bits128` and similar if it represents fixed-width data without refs
 */
