//unreachable-6
enum Role {
    User,
    Admin,
}

@method_id(101)
fun checkRole(r: Role) {
    match (r) {
        Role.User => {
            return 1;
        }
        Role.Admin => {
            return 100;
        }
    }
    throw 123;
}

@method_id(102)
fun checkBool(b: bool) {
    match (b) {
        true => return 1,
        false => return 100,
    } 
    throw 456;
}

fun main() {}

/**
@testcase | 101 | 1  | 100
@testcase | 102 | -1 | 1
@testcase | 102 | 0  | 100
@testcase | 102 | 1  | 100

@stderr warning: unreachable code
@stderr throw 123;
@stderr throw 456;
 */
