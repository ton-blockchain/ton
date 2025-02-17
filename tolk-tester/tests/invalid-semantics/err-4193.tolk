struct User {
    id: int;
}

fun cantInstantiateNonGenericStruct() {
    var u = User<int> { id: 3 };
}

/**
@compilation_should_fail
@stderr generic T not expected here
@stderr User<int>
 */
