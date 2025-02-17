struct User {
    id: int;
}

type UserAlias = User;

fun cantInstantiateNonGenericStruct() {
    var u = UserAlias<int> { id: 3 };
}

/**
@compilation_should_fail
@stderr generic T not expected here
@stderr UserAlias<int>
 */
