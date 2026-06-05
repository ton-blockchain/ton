fun main() {
    var a = true;
    debug.printString(reflect.typeNameOfObject(a))
}

/**
@compilation_should_fail
@stderr unknown type name `reflect`
 */
