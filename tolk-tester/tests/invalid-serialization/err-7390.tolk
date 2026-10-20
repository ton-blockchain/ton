type MyInt = int

fun MyInt.packToBuilder<T>(self, mutate b: builder) {

}

fun main() {
    var x: MyInt = 42;
    var s: slice = x.toCell().beginParse();
    var y: MyInt = MyInt.fromSlice(s);
    return y;
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `MyInt`
@stderr because type `int` is not serializable
@stderr x.toCell()
@stderr MyInt.fromSlice(s)
 */
