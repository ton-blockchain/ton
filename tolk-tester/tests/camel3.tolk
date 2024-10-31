// Here we test that if you declare a wrapper like
// >  builder beginCell() { return begin_cell(); }
// but use it NOT only as a direct call, BUT as a 1-st class function
// (save to a variable, return from a function, etc.)
// it also works, since a function becomes codegenerated (though direct calls are expectedly inlined).

fun myBeginCell(): builder { return beginCell(); }
fun myEndCell(b: builder): cell { return endCell(b); }
fun myStoreRef(b: builder, c: cell): builder { return storeRef(b, c); }
fun myStoreUint3(i: int, bw: int, b: builder): builder { return storeUint(b, i, bw); }

fun computeDataSize2(maxCells: int, c: cell): (int, int, int) { return calculateCellSizeStrict(c, maxCells); }

fun myEmptyTuple(): tuple { return createEmptyTuple(); }
fun myTuplePush<X>(t: tuple, value: X): tuple { return tuplePush(t, value); }
fun ~myTuplePush<X>(t: tuple, value: X): (tuple, ()) { return ~tuplePush(t, value); }
fun myTupleGetFirst<X>(t: tuple): X { return tupleFirst(t); }


@inline
fun getBeginEnd(): (auto, auto) {
    return (myBeginCell, myEndCell);
}

fun begAndStore(beg: auto, store: auto, x: int): builder {
    return store(x, 8, beg());
}

fun test1(): (int, int, int) {
    var (_, computer) = (0, computeDataSize2);
    var (beg, end) = getBeginEnd();

    var t: tuple = myEmptyTuple();
    t~myTuplePush(myStoreRef);
    var refStorer = myTupleGetFirst(t);

    var x: int = 1;
    var y: int = 1;
    var to_be_ref: cell = myBeginCell().myEndCell();
    var in_c: builder = begAndStore(beg, myStoreUint3, 123);
    in_c = refStorer(in_c, to_be_ref);
    var (a, b, c) = computer(10, end(in_c));
    return (a, b + x, c + y);
}

fun main(): (int, int, int) {
    return test1();
}

/**
    method_id | in | out
@testcase | 0  |    | 2 9 2

@fif_codegen DECLPROC myBeginCell
@fif_codegen DECLPROC computeDataSize2

@fif_codegen
"""
  myStoreUint3 PROC:<{
    //  i bw b
    SWAP        //  i b bw
    STUX        //  _3
  }>
"""

@fif_codegen
"""
  myStoreRef PROC:<{
    //  b c
    SWAP        //  c b
    STREF       //  _2
  }>
"""

@fif_codegen
"""
    CONT:<{
      computeDataSize2 CALLDICT
    }>  //  computer
    getBeginEnd INLINECALLDICT  //  computer beg end
    NIL //  computer beg end t
    ...
    NEWC        //  computer beg end refStorer _19
    ENDC        //  computer beg end refStorer to_be_ref
    ...
    CONT:<{
      myStoreUint3 CALLDICT
    }>
    ...
    begAndStore CALLDICT        //  computer to_be_ref end refStorer in_c
"""

@fif_codegen_avoid myEmptyTuple
@fif_codegen_avoid myTuplePush
*/
