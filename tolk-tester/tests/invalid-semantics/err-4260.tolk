struct User {
    id: int;
}

type UserAlias = User;

fun cantInstantiateNonGenericStruct() {
    var u = UserAlias<int> { id: 3 };
}

/**
@compilation_should_fail
@stderr type `UserAlias` is not generic
@stderr UserAlias<int>
 */
