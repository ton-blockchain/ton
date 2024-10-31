// A part of standard library for Tolk
tolk 0.6

/**
    Dictionaries are represented as `cell` data type (cells can store anything, dicts in particular).
    Currently, they have very low-level API very close to TVM internals.
    Most of functions are duplicated for three common cases:
    - iDict* - dicts with signed integer keys
    - uDict* - dicts with unsigned integer keys
    - sDict* - dicts with arbitrary slice keys
    When accessing a dict element, you should not only provide a key, but provide keyLen,
  since for optimization, for optimization, key length is not stored in the dictionary itself.
 */

/// Creates an empty dictionary, which is actually a null value. Equivalent to PUSHNULL
@pure
fun createEmptyDict(): cell
    asm "NEWDICT";

/// Checks whether a dictionary is empty.
@pure
fun dictIsEmpty(c: cell): int
    asm "DICTEMPTY";


@pure
fun iDictGet(dict: cell, keyLen: int, key: int): (slice, int)
    asm(key dict keyLen) "DICTIGET" "NULLSWAPIFNOT";

@pure
fun uDictGet(dict: cell, keyLen: int, key: int): (slice, int)
    asm(key dict keyLen) "DICTUGET" "NULLSWAPIFNOT";

@pure
fun sDictGet(dict: cell, keyLen: int, key: slice): (slice, int)
    asm(key dict keyLen) "DICTGET" "NULLSWAPIFNOT";


@pure
fun iDictSet(dict: cell, keyLen: int, key: int, value: slice): cell
    asm(value key dict keyLen) "DICTISET";

@pure
fun ~iDictSet(dict: cell, keyLen: int, key: int, value: slice): (cell, ())
    asm(value key dict keyLen) "DICTISET";

@pure
fun uDictSet(dict: cell, keyLen: int, key: int, value: slice): cell
    asm(value key dict keyLen) "DICTUSET";

@pure
fun ~uDictSet(dict: cell, keyLen: int, key: int, value: slice): (cell, ())
    asm(value key dict keyLen) "DICTUSET";

@pure
fun sDictSet(dict: cell, keyLen: int, key: slice, value: slice): cell
    asm(value key dict keyLen) "DICTSET";

@pure
fun ~sDictSet(dict: cell, keyLen: int, key: slice, value: slice): (cell, ())
    asm(value key dict keyLen) "DICTSET";


@pure
fun iDictSetRef(dict: cell, keyLen: int, key: int, value: cell): cell
    asm(value key dict keyLen) "DICTISETREF";

@pure
fun ~iDictSetRef(dict: cell, keyLen: int, key: int, value: cell): (cell, ())
    asm(value key dict keyLen) "DICTISETREF";

@pure
fun uDictSetRef(dict: cell, keyLen: int, key: int, value: cell): cell
    asm(value key dict keyLen) "DICTUSETREF";

@pure
fun ~uDictSetRef(dict: cell, keyLen: int, key: int, value: cell): (cell, ())
    asm(value key dict keyLen) "DICTUSETREF";

@pure
fun sDictSetRef(dict: cell, keyLen: int, key: slice, value: cell): cell
    asm(value key dict keyLen) "DICTSETREF";

@pure
fun ~sDictSetRef(dict: cell, keyLen: int, key: slice, value: cell): (cell, ())
    asm(value key dict keyLen) "DICTSETREF";


@pure
fun iDictSetIfNotExists(dict: cell, keyLen: int, key: int, value: slice): (cell, int)
    asm(value key dict keyLen) "DICTIADD";

@pure
fun ~iDictSetIfNotExists(dict: cell, keyLen: int, key: int, value: slice): (cell, int)
    asm(value key dict keyLen) "DICTIADD";

@pure
fun uDictSetIfNotExists(dict: cell, keyLen: int, key: int, value: slice): (cell, int)
    asm(value key dict keyLen) "DICTUADD";

@pure
fun ~uDictSetIfNotExists(dict: cell, keyLen: int, key: int, value: slice): (cell, int)
    asm(value key dict keyLen) "DICTUADD";


@pure
fun iDictSetIfExists(dict: cell, keyLen: int, key: int, value: slice): (cell, int)
    asm(value key dict keyLen) "DICTIREPLACE";

@pure
fun ~iDictSetIfExists(dict: cell, keyLen: int, key: int, value: slice): (cell, int)
    asm(value key dict keyLen) "DICTIREPLACE";

@pure
fun uDictSetIfExists(dict: cell, keyLen: int, key: int, value: slice): (cell, int)
    asm(value key dict keyLen) "DICTUREPLACE";

@pure
fun ~uDictSetIfExists(dict: cell, keyLen: int, key: int, value: slice): (cell, int)
    asm(value key dict keyLen) "DICTUREPLACE";


@pure
fun iDictGetRef(dict: cell, keyLen: int, key: int): (cell, int)
    asm(key dict keyLen) "DICTIGETREF" "NULLSWAPIFNOT";

@pure
fun uDictGetRef(dict: cell, keyLen: int, key: int): (cell, int)
    asm(key dict keyLen) "DICTUGETREF" "NULLSWAPIFNOT";

@pure
fun sDictGetRef(dict: cell, keyLen: int, key: slice): (cell, int)
    asm(key dict keyLen) "DICTGETREF" "NULLSWAPIFNOT";


@pure
fun iDictGetRefOrNull(dict: cell, keyLen: int, key: int): cell
    asm(key dict keyLen) "DICTIGETOPTREF";

@pure
fun uDictGetRefOrNull(dict: cell, keyLen: int, key: int): cell
    asm(key dict keyLen) "DICTUGETOPTREF";

@pure
fun sDictGetRefOrNull(dict: cell, keyLen: int, key: slice): cell
    asm(key dict keyLen) "DICTGETOPTREF";


@pure
fun iDictDelete(dict: cell, keyLen: int, key: int): (cell, int)
    asm(key dict keyLen) "DICTIDEL";

@pure
fun ~iDictDelete(dict: cell, keyLen: int, key: int): (cell, int)
    asm(key dict keyLen) "DICTIDEL";

@pure
fun uDictDelete(dict: cell, keyLen: int, key: int): (cell, int)
    asm(key dict keyLen) "DICTUDEL";

@pure
fun ~uDictDelete(dict: cell, keyLen: int, key: int): (cell, int)
    asm(key dict keyLen) "DICTUDEL";

@pure
fun sDictDelete(dict: cell, keyLen: int, key: slice): (cell, int)
    asm(key dict keyLen) "DICTDEL";

@pure
fun ~sDictDelete(dict: cell, keyLen: int, key: slice): (cell, int)
    asm(key dict keyLen) "DICTDEL";


@pure
fun iDictSetAndGet(dict: cell, keyLen: int, key: int, value: slice): (cell, slice, int)
    asm(value key dict keyLen) "DICTISETGET" "NULLSWAPIFNOT";

@pure
fun ~iDictSetAndGet(dict: cell, keyLen: int, key: int, value: slice): (cell, (slice, int))
    asm(value key dict keyLen) "DICTISETGET" "NULLSWAPIFNOT";

@pure
fun uDictSetAndGet(dict: cell, keyLen: int, key: int, value: slice): (cell, slice, int)
    asm(value key dict keyLen) "DICTUSETGET" "NULLSWAPIFNOT";

@pure
fun ~uDictSetAndGet(dict: cell, keyLen: int, key: int, value: slice): (cell, (slice, int))
    asm(value key dict keyLen) "DICTUSETGET" "NULLSWAPIFNOT";

@pure
fun sDictSetAndGet(dict: cell, keyLen: int, key: slice, value: slice): (cell, slice, int)
    asm(value key dict keyLen) "DICTSETGET" "NULLSWAPIFNOT";

@pure
fun ~sDictSetAndGet(dict: cell, keyLen: int, key: slice, value: slice): (cell, (slice, int))
    asm(value key dict keyLen) "DICTSETGET" "NULLSWAPIFNOT";


@pure
fun iDictSetAndGetPreviousRefOrNull(dict: cell, keyLen: int, key: int, value: cell): (cell, cell)
    asm(value key dict keyLen) "DICTISETGETOPTREF";

@pure
fun ~iDictSetAndGetPreviousRefOrNull(dict: cell, keyLen: int, key: int, value: cell): (cell, cell)
    asm(value key dict keyLen) "DICTISETGETOPTREF";

@pure
fun uDictSetAndGetPreviousRefOrNull(dict: cell, keyLen: int, key: int, value: cell): (cell, cell)
    asm(value key dict keyLen) "DICTUSETGETOPTREF";

@pure
fun ~uDictSetAndGetPreviousRefOrNull(dict: cell, keyLen: int, key: int, value: cell): (cell, cell)
    asm(value key dict keyLen) "DICTUSETGETOPTREF";


@pure
fun iDictDeleteAndGet(dict: cell, keyLen: int, key: int): (cell, slice, int)
    asm(key dict keyLen) "DICTIDELGET" "NULLSWAPIFNOT";

@pure
fun ~iDictDeleteAndGet(dict: cell, keyLen: int, key: int): (cell, (slice, int))
    asm(key dict keyLen) "DICTIDELGET" "NULLSWAPIFNOT";

@pure
fun uDictDeleteAndGet(dict: cell, keyLen: int, key: int): (cell, slice, int)
    asm(key dict keyLen) "DICTUDELGET" "NULLSWAPIFNOT";

@pure
fun ~uDictDeleteAndGet(dict: cell, keyLen: int, key: int): (cell, (slice, int))
    asm(key dict keyLen) "DICTUDELGET" "NULLSWAPIFNOT";

@pure
fun sDictDeleteAndGet(dict: cell, keyLen: int, key: slice): (cell, slice, int)
    asm(key dict keyLen) "DICTDELGET" "NULLSWAPIFNOT";

@pure
fun ~sDictDeleteAndGet(dict: cell, keyLen: int, key: slice): (cell, (slice, int))
    asm(key dict keyLen) "DICTDELGET" "NULLSWAPIFNOT";


@pure
fun iDictSetBuilder(dict: cell, keyLen: int, key: int, value: builder): cell
    asm(value key dict keyLen) "DICTISETB";

@pure
fun ~iDictSetBuilder(dict: cell, keyLen: int, key: int, value: builder): (cell, ())
    asm(value key dict keyLen) "DICTISETB";

@pure
fun uDictSetBuilder(dict: cell, keyLen: int, key: int, value: builder): cell
    asm(value key dict keyLen) "DICTUSETB";

@pure
fun ~uDictSetBuilder(dict: cell, keyLen: int, key: int, value: builder): (cell, ())
    asm(value key dict keyLen) "DICTUSETB";

@pure
fun sDictSetBuilder(dict: cell, keyLen: int, key: slice, value: builder): cell
    asm(value key dict keyLen) "DICTSETB";

@pure
fun ~sDictSetBuilder(dict: cell, keyLen: int, key: slice, value: builder): (cell, ())
    asm(value key dict keyLen) "DICTSETB";


@pure
fun iDictSetBuilderIfNotExists(dict: cell, keyLen: int, key: int, value: builder): (cell, int)
    asm(value key dict keyLen) "DICTIADDB";

@pure
fun ~iDictSetBuilderIfNotExists(dict: cell, keyLen: int, key: int, value: builder): (cell, int)
    asm(value key dict keyLen) "DICTIADDB";

@pure
fun uDictSetBuilderIfNotExists(dict: cell, keyLen: int, key: int, value: builder): (cell, int)
    asm(value key dict keyLen) "DICTUADDB";

@pure
fun ~uDictSetBuilderIfNotExists(dict: cell, keyLen: int, key: int, value: builder): (cell, int)
    asm(value key dict keyLen) "DICTUADDB";

@pure
fun iDictSetBuilderIfExists(dict: cell, keyLen: int, key: int, value: builder): (cell, int)
    asm(value key dict keyLen) "DICTIREPLACEB";

@pure
fun ~iDictSetBuilderIfExists(dict: cell, keyLen: int, key: int, value: builder): (cell, int)
    asm(value key dict keyLen) "DICTIREPLACEB";

@pure
fun uDictSetBuilderIfExists(dict: cell, keyLen: int, key: int, value: builder): (cell, int)
    asm(value key dict keyLen) "DICTUREPLACEB";

@pure
fun ~uDictSetBuilderIfExists(dict: cell, keyLen: int, key: int, value: builder): (cell, int)
    asm(value key dict keyLen) "DICTUREPLACEB";


@pure
fun iDictDeleteFirstAndGet(dict: cell, keyLen: int): (cell, int, slice, int)
    asm(-> 0 2 1 3) "DICTIREMMIN" "NULLSWAPIFNOT2";

@pure
fun ~iDictDeleteFirstAndGet(dict: cell, keyLen: int): (cell, (int, slice, int))
    asm(-> 0 2 1 3) "DICTIREMMIN" "NULLSWAPIFNOT2";

@pure
fun uDictDeleteFirstAndGet(dict: cell, keyLen: int): (cell, int, slice, int)
    asm(-> 0 2 1 3) "DICTUREMMIN" "NULLSWAPIFNOT2";

@pure
fun ~uDictDeleteFirstAndGet(dict: cell, keyLen: int): (cell, (int, slice, int))
    asm(-> 0 2 1 3) "DICTUREMMIN" "NULLSWAPIFNOT2";

@pure
fun sDictDeleteFirstAndGet(dict: cell, keyLen: int): (cell, slice, slice, int)
    asm(-> 0 2 1 3) "DICTREMMIN" "NULLSWAPIFNOT2";

@pure
fun ~sDictDeleteFirstAndGet(dict: cell, keyLen: int): (cell, (slice, slice, int))
    asm(-> 0 2 1 3) "DICTREMMIN" "NULLSWAPIFNOT2";


@pure
fun iDictDeleteLastAndGet(dict: cell, keyLen: int): (cell, int, slice, int)
    asm(-> 0 2 1 3) "DICTIREMMAX" "NULLSWAPIFNOT2";

@pure
fun ~iDictDeleteLastAndGet(dict: cell, keyLen: int): (cell, (int, slice, int))
    asm(-> 0 2 1 3) "DICTIREMMAX" "NULLSWAPIFNOT2";

@pure
fun uDictDeleteLastAndGet(dict: cell, keyLen: int): (cell, int, slice, int)
    asm(-> 0 2 1 3) "DICTUREMMAX" "NULLSWAPIFNOT2";

@pure
fun ~uDictDeleteLastAndGet(dict: cell, keyLen: int): (cell, (int, slice, int))
    asm(-> 0 2 1 3) "DICTUREMMAX" "NULLSWAPIFNOT2";

@pure
fun sDictDeleteLastAndGet(dict: cell, keyLen: int): (cell, slice, slice, int)
    asm(-> 0 2 1 3) "DICTREMMAX" "NULLSWAPIFNOT2";

@pure
fun ~sDictDeleteLastAndGet(dict: cell, keyLen: int): (cell, (slice, slice, int))
    asm(-> 0 2 1 3) "DICTREMMAX" "NULLSWAPIFNOT2";


@pure
fun iDictGetFirst(dict: cell, keyLen: int): (int, slice, int)
    asm (-> 1 0 2) "DICTIMIN" "NULLSWAPIFNOT2";

@pure
fun uDictGetFirst(dict: cell, keyLen: int): (int, slice, int)
    asm (-> 1 0 2) "DICTUMIN" "NULLSWAPIFNOT2";

@pure
fun sDictGetFirst(dict: cell, keyLen: int): (slice, slice, int)
    asm (-> 1 0 2) "DICTMIN" "NULLSWAPIFNOT2";

@pure
fun iDictGetFirstAsRef(dict: cell, keyLen: int): (int, cell, int)
    asm (-> 1 0 2) "DICTIMINREF" "NULLSWAPIFNOT2";

@pure
fun uDictGetFirstAsRef(dict: cell, keyLen: int): (int, cell, int)
    asm (-> 1 0 2) "DICTUMINREF" "NULLSWAPIFNOT2";

@pure
fun sDictGetFirstAsRef(dict: cell, keyLen: int): (slice, cell, int)
    asm (-> 1 0 2) "DICTMINREF" "NULLSWAPIFNOT2";


@pure
fun iDictGetLast(dict: cell, keyLen: int): (int, slice, int)
    asm (-> 1 0 2) "DICTIMAX" "NULLSWAPIFNOT2";

@pure
fun uDictGetLast(dict: cell, keyLen: int): (int, slice, int)
    asm (-> 1 0 2) "DICTUMAX" "NULLSWAPIFNOT2";

@pure
fun sDictGetLast(dict: cell, keyLen: int): (slice, slice, int)
    asm (-> 1 0 2) "DICTMAX" "NULLSWAPIFNOT2";

@pure
fun iDictGetLastAsRef(dict: cell, keyLen: int): (int, cell, int)
    asm (-> 1 0 2) "DICTIMAXREF" "NULLSWAPIFNOT2";

@pure
fun uDictGetLastAsRef(dict: cell, keyLen: int): (int, cell, int)
    asm (-> 1 0 2) "DICTUMAXREF" "NULLSWAPIFNOT2";

@pure
fun sDictGetLastAsRef(dict: cell, keyLen: int): (slice, cell, int)
    asm (-> 1 0 2) "DICTMAXREF" "NULLSWAPIFNOT2";


@pure
fun iDictGetNext(dict: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot dict keyLen -> 1 0 2) "DICTIGETNEXT" "NULLSWAPIFNOT2";

@pure
fun uDictGetNext(dict: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot dict keyLen -> 1 0 2) "DICTUGETNEXT" "NULLSWAPIFNOT2";

@pure
fun iDictGetNextOrEqual(dict: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot dict keyLen -> 1 0 2) "DICTIGETNEXTEQ" "NULLSWAPIFNOT2";

@pure
fun uDictGetNextOrEqual(dict: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot dict keyLen -> 1 0 2) "DICTUGETNEXTEQ" "NULLSWAPIFNOT2";


@pure
fun iDictGetPrev(dict: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot dict keyLen -> 1 0 2) "DICTIGETPREV" "NULLSWAPIFNOT2";

@pure
fun uDictGetPrev(dict: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot dict keyLen -> 1 0 2) "DICTUGETPREV" "NULLSWAPIFNOT2";

@pure
fun iDictGetPrevOrEqual(dict: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot dict keyLen -> 1 0 2) "DICTIGETPREVEQ" "NULLSWAPIFNOT2";

@pure
fun uDictGetPrevOrEqual(dict: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot dict keyLen -> 1 0 2) "DICTUGETPREVEQ" "NULLSWAPIFNOT2";


/**
 Prefix dictionary primitives.
 */

@pure
fun prefixDictGet(dict: cell, keyLen: int, key: slice): (slice, slice, slice, int)
    asm(key dict keyLen) "PFXDICTGETQ" "NULLSWAPIFNOT2";

@pure
fun prefixDictSet(dict: cell, keyLen: int, key: slice, value: slice): (cell, int)
    asm(value key dict keyLen) "PFXDICTSET";

@pure
fun prefixDictDelete(dict: cell, keyLen: int, key: slice): (cell, int)
    asm(key dict keyLen) "PFXDICTDEL";
