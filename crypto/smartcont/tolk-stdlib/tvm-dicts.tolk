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
fun dictIsEmpty(self: cell): int
    asm "DICTEMPTY";


@pure
fun iDictGet(self: cell, keyLen: int, key: int): (slice, int)
    asm(key self keyLen) "DICTIGET" "NULLSWAPIFNOT";

@pure
fun uDictGet(self: cell, keyLen: int, key: int): (slice, int)
    asm(key self keyLen) "DICTUGET" "NULLSWAPIFNOT";

@pure
fun sDictGet(self: cell, keyLen: int, key: slice): (slice, int)
    asm(key self keyLen) "DICTGET" "NULLSWAPIFNOT";


@pure
fun iDictSet(mutate self: cell, keyLen: int, key: int, value: slice): void
    asm(value key self keyLen) "DICTISET";

@pure
fun uDictSet(mutate self: cell, keyLen: int, key: int, value: slice): void
    asm(value key self keyLen) "DICTUSET";

@pure
fun sDictSet(mutate self: cell, keyLen: int, key: slice, value: slice): void
    asm(value key self keyLen) "DICTSET";


@pure
fun iDictSetRef(mutate self: cell, keyLen: int, key: int, value: cell): void
    asm(value key self keyLen) "DICTISETREF";

@pure
fun uDictSetRef(mutate self: cell, keyLen: int, key: int, value: cell): void
    asm(value key self keyLen) "DICTUSETREF";

@pure
fun sDictSetRef(mutate self: cell, keyLen: int, key: slice, value: cell): void
    asm(value key self keyLen) "DICTSETREF";


@pure
fun iDictSetIfNotExists(mutate self: cell, keyLen: int, key: int, value: slice): int
    asm(value key self keyLen) "DICTIADD";

@pure
fun uDictSetIfNotExists(mutate self: cell, keyLen: int, key: int, value: slice): int
    asm(value key self keyLen) "DICTUADD";


@pure
fun iDictSetIfExists(mutate self: cell, keyLen: int, key: int, value: slice): int
    asm(value key self keyLen) "DICTIREPLACE";

@pure
fun uDictSetIfExists(mutate self: cell, keyLen: int, key: int, value: slice): int
    asm(value key self keyLen) "DICTUREPLACE";


@pure
fun iDictGetRef(self: cell, keyLen: int, key: int): (cell, int)
    asm(key self keyLen) "DICTIGETREF" "NULLSWAPIFNOT";

@pure
fun uDictGetRef(self: cell, keyLen: int, key: int): (cell, int)
    asm(key self keyLen) "DICTUGETREF" "NULLSWAPIFNOT";

@pure
fun sDictGetRef(self: cell, keyLen: int, key: slice): (cell, int)
    asm(key self keyLen) "DICTGETREF" "NULLSWAPIFNOT";


@pure
fun iDictGetRefOrNull(self: cell, keyLen: int, key: int): cell
    asm(key self keyLen) "DICTIGETOPTREF";

@pure
fun uDictGetRefOrNull(self: cell, keyLen: int, key: int): cell
    asm(key self keyLen) "DICTUGETOPTREF";

@pure
fun sDictGetRefOrNull(self: cell, keyLen: int, key: slice): cell
    asm(key self keyLen) "DICTGETOPTREF";


@pure
fun iDictDelete(mutate self: cell, keyLen: int, key: int): int
    asm(key self keyLen) "DICTIDEL";

@pure
fun uDictDelete(mutate self: cell, keyLen: int, key: int): int
    asm(key self keyLen) "DICTUDEL";

@pure
fun sDictDelete(mutate self: cell, keyLen: int, key: slice): int
    asm(key self keyLen) "DICTDEL";


@pure
fun iDictSetAndGet(mutate self: cell, keyLen: int, key: int, value: slice): (slice, int)
    asm(value key self keyLen) "DICTISETGET" "NULLSWAPIFNOT";

@pure
fun uDictSetAndGet(mutate self: cell, keyLen: int, key: int, value: slice): (slice, int)
    asm(value key self keyLen) "DICTUSETGET" "NULLSWAPIFNOT";

@pure
fun sDictSetAndGet(mutate self: cell, keyLen: int, key: slice, value: slice): (slice, int)
    asm(value key self keyLen) "DICTSETGET" "NULLSWAPIFNOT";


@pure
fun iDictSetAndGetRefOrNull(mutate self: cell, keyLen: int, key: int, value: cell): cell
    asm(value key self keyLen) "DICTISETGETOPTREF";

@pure
fun uDictSetAndGetRefOrNull(mutate self: cell, keyLen: int, key: int, value: cell): cell
    asm(value key self keyLen) "DICTUSETGETOPTREF";


@pure
fun iDictDeleteAndGet(mutate self: cell, keyLen: int, key: int): (slice, int)
    asm(key self keyLen) "DICTIDELGET" "NULLSWAPIFNOT";

@pure
fun uDictDeleteAndGet(mutate self: cell, keyLen: int, key: int): (slice, int)
    asm(key self keyLen) "DICTUDELGET" "NULLSWAPIFNOT";

@pure
fun sDictDeleteAndGet(mutate self: cell, keyLen: int, key: slice): (slice, int)
    asm(key self keyLen) "DICTDELGET" "NULLSWAPIFNOT";


@pure
fun iDictSetBuilder(mutate self: cell, keyLen: int, key: int, value: builder): void
    asm(value key self keyLen) "DICTISETB";

@pure
fun uDictSetBuilder(mutate self: cell, keyLen: int, key: int, value: builder): void
    asm(value key self keyLen) "DICTUSETB";

@pure
fun sDictSetBuilder(mutate self: cell, keyLen: int, key: slice, value: builder): void
    asm(value key self keyLen) "DICTSETB";


@pure
fun iDictSetBuilderIfNotExists(mutate self: cell, keyLen: int, key: int, value: builder): int
    asm(value key self keyLen) "DICTIADDB";

@pure
fun uDictSetBuilderIfNotExists(mutate self: cell, keyLen: int, key: int, value: builder): int
    asm(value key self keyLen) "DICTUADDB";

@pure
fun iDictSetBuilderIfExists(mutate self: cell, keyLen: int, key: int, value: builder): int
    asm(value key self keyLen) "DICTIREPLACEB";

@pure
fun uDictSetBuilderIfExists(mutate self: cell, keyLen: int, key: int, value: builder): int
    asm(value key self keyLen) "DICTUREPLACEB";


@pure
fun iDictDeleteFirstAndGet(mutate self: cell, keyLen: int): (int, slice, int)
    asm(-> 0 2 1 3) "DICTIREMMIN" "NULLSWAPIFNOT2";

@pure
fun uDictDeleteFirstAndGet(mutate self: cell, keyLen: int): (int, slice, int)
    asm(-> 0 2 1 3) "DICTUREMMIN" "NULLSWAPIFNOT2";

@pure
fun sDictDeleteFirstAndGet(mutate self: cell, keyLen: int): (slice, slice, int)
    asm(-> 0 2 1 3) "DICTREMMIN" "NULLSWAPIFNOT2";


@pure
fun iDictDeleteLastAndGet(mutate self: cell, keyLen: int): (int, slice, int)
    asm(-> 0 2 1 3) "DICTIREMMAX" "NULLSWAPIFNOT2";

@pure
fun uDictDeleteLastAndGet(mutate self: cell, keyLen: int): (int, slice, int)
    asm(-> 0 2 1 3) "DICTUREMMAX" "NULLSWAPIFNOT2";

@pure
fun sDictDeleteLastAndGet(mutate self: cell, keyLen: int): (slice, slice, int)
    asm(-> 0 2 1 3) "DICTREMMAX" "NULLSWAPIFNOT2";


@pure
fun iDictGetFirst(self: cell, keyLen: int): (int, slice, int)
    asm (-> 1 0 2) "DICTIMIN" "NULLSWAPIFNOT2";

@pure
fun uDictGetFirst(self: cell, keyLen: int): (int, slice, int)
    asm (-> 1 0 2) "DICTUMIN" "NULLSWAPIFNOT2";

@pure
fun sDictGetFirst(self: cell, keyLen: int): (slice, slice, int)
    asm (-> 1 0 2) "DICTMIN" "NULLSWAPIFNOT2";

@pure
fun iDictGetFirstAsRef(self: cell, keyLen: int): (int, cell, int)
    asm (-> 1 0 2) "DICTIMINREF" "NULLSWAPIFNOT2";

@pure
fun uDictGetFirstAsRef(self: cell, keyLen: int): (int, cell, int)
    asm (-> 1 0 2) "DICTUMINREF" "NULLSWAPIFNOT2";

@pure
fun sDictGetFirstAsRef(self: cell, keyLen: int): (slice, cell, int)
    asm (-> 1 0 2) "DICTMINREF" "NULLSWAPIFNOT2";


@pure
fun iDictGetLast(self: cell, keyLen: int): (int, slice, int)
    asm (-> 1 0 2) "DICTIMAX" "NULLSWAPIFNOT2";

@pure
fun uDictGetLast(self: cell, keyLen: int): (int, slice, int)
    asm (-> 1 0 2) "DICTUMAX" "NULLSWAPIFNOT2";

@pure
fun sDictGetLast(self: cell, keyLen: int): (slice, slice, int)
    asm (-> 1 0 2) "DICTMAX" "NULLSWAPIFNOT2";

@pure
fun iDictGetLastAsRef(self: cell, keyLen: int): (int, cell, int)
    asm (-> 1 0 2) "DICTIMAXREF" "NULLSWAPIFNOT2";

@pure
fun uDictGetLastAsRef(self: cell, keyLen: int): (int, cell, int)
    asm (-> 1 0 2) "DICTUMAXREF" "NULLSWAPIFNOT2";

@pure
fun sDictGetLastAsRef(self: cell, keyLen: int): (slice, cell, int)
    asm (-> 1 0 2) "DICTMAXREF" "NULLSWAPIFNOT2";


@pure
fun iDictGetNext(self: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETNEXT" "NULLSWAPIFNOT2";

@pure
fun uDictGetNext(self: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETNEXT" "NULLSWAPIFNOT2";

@pure
fun iDictGetNextOrEqual(self: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETNEXTEQ" "NULLSWAPIFNOT2";

@pure
fun uDictGetNextOrEqual(self: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETNEXTEQ" "NULLSWAPIFNOT2";


@pure
fun iDictGetPrev(self: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETPREV" "NULLSWAPIFNOT2";

@pure
fun uDictGetPrev(self: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETPREV" "NULLSWAPIFNOT2";

@pure
fun iDictGetPrevOrEqual(self: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETPREVEQ" "NULLSWAPIFNOT2";

@pure
fun uDictGetPrevOrEqual(self: cell, keyLen: int, pivot: int): (int, slice, int)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETPREVEQ" "NULLSWAPIFNOT2";


/**
 Prefix dictionary primitives.
 */

@pure
fun prefixDictGet(self: cell, keyLen: int, key: slice): (slice, slice, slice, int)
    asm(key self keyLen) "PFXDICTGETQ" "NULLSWAPIFNOT2";

@pure
fun prefixDictSet(mutate self: cell, keyLen: int, key: slice, value: slice): int
    asm(value key self keyLen) "PFXDICTSET";

@pure
fun prefixDictDelete(mutate self: cell, keyLen: int, key: slice): int
    asm(key self keyLen) "PFXDICTDEL";
