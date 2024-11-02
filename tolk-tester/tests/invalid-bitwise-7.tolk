fun main() {
    var c = x && y || x && y;
}

/**
@compilation_should_fail
@stderr mixing && with || without parenthesis
*/
