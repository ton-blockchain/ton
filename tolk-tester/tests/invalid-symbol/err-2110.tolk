fun main() {
    var a = address("0:ca6e321c7cce9ecedf0a8ca2492ec8592494aa5fb5ce0387dff96ef6af982a3e");
    return a.isInternal();
}

/**
@compilation_should_fail
@stderr method `isInternal` not found for type `address`
@stderr (but it exists for type `any_address`)
 */
