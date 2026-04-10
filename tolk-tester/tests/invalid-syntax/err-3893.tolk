fun main() {
    match (123) {
        123 => assert (true) throw 123;     // error here, `;`
        456 => 1,
    }
}

/**
@compilation_should_fail
@stderr expected <type> or <expression> in `match` before `=>`
 */
