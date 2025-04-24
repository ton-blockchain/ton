
fun main(): int

// inside a comment, /* doesn't start a new one
/* but if // is inside, a comment may end at this line*/ {
    var cc = "a string may contain /* or // or /*, not parsed";
    // return 1;
    return get10() + /*
    traditional comment /* may not be nested
    // line comment
    // ends */1 +
    1;
    /* moreover, different comment styles
        may be used for opening and closing
    */
}

/***
    first line
     //two-lined*/

@method_id(10)
fun get10(): int {
    return 10;
}


/**
@testcase | 0  | | 12
@testcase | 10 | | 10
*/
