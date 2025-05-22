struct User {
    id: int;
}

fun cantInstantiateNonGenericStruct() {
    var u = User<int> { id: 3 };
}

/**
@compilation_should_fail
@stderr type `User` is not generic
@stderr User<int>
 */
