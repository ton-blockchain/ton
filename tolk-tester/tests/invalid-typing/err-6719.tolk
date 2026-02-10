import "@stdlib/lisp-lists"

fun err() {
    var l1 = [] as lisp_list<int>;
    var l2 = [] as lisp_list<int?>;

    // lisp_list<T> is not built-in, such types are not assignable
    // (in general, it will require runtime transition, e.g. from `null` to `Point?`)
    l2 = l1;
}

/**
@compilation_should_fail
@stderr can not assign `lisp_list<int>` to variable of type `lisp_list<int?>`
 */
