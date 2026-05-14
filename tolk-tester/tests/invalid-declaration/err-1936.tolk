contract GG {
    thrownErrors: Color | DD
}

struct DD {
}

enum Color {
    red,
}

fun main() {

}

/**
@compilation_should_fail
@stderr `thrownErrors` must be an enum type, got `DD`
 */
