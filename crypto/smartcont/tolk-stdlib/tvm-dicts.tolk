// A part of standard library for Tolk
tolk 0.12

/**
    Dictionaries are represented as `cell` data type (cells can store anything, dicts in particular).
    Currently, they have very low-level API very close to TVM internals.
    Most of functions are duplicated for three common cases:
    - iDict* - dicts with signed integer keys
    - uDict* - dicts with unsigned integer keys
    - sDict* - dicts with arbitrary slice keys
    When accessing a dict element, you should not only provide a key, but provide keyLen,
  since for optimization, key length is not stored in the dictionary itself.
    Every dictionary object (`self` parameter) can be null. TVM NULL is essentially "empty dictionary".
 */

/// In @stdlib/common.tolk, there is a type alias:
/// `type dict = cell?;`
/// For clarity, we use "dict" instead of a "cell?" where a cell-dictionary is assumed.

/// Creates an empty dictionary, which is actually a null value. Equivalent to PUSHNULL
@pure
fun createEmptyDict(): dict
    asm "NEWDICT";

/// Checks whether a dictionary is empty.
@pure
fun dict.dictIsEmpty(self): bool
    asm "DICTEMPTY";


@pure
fun dict.iDictGet(self, keyLen: int, key: int): (slice?, bool)
    asm(key self keyLen) "DICTIGET" "NULLSWAPIFNOT";

@pure
fun dict.uDictGet(self, keyLen: int, key: int): (slice?, bool)
    asm(key self keyLen) "DICTUGET" "NULLSWAPIFNOT";

@pure
fun dict.sDictGet(self, keyLen: int, key: slice): (slice?, bool)
    asm(key self keyLen) "DICTGET" "NULLSWAPIFNOT";


@pure
fun dict.iDictSet(mutate self, keyLen: int, key: int, value: slice): void
    asm(value key self keyLen) "DICTISET";

@pure
fun dict.uDictSet(mutate self, keyLen: int, key: int, value: slice): void
    asm(value key self keyLen) "DICTUSET";

@pure
fun dict.sDictSet(mutate self, keyLen: int, key: slice, value: slice): void
    asm(value key self keyLen) "DICTSET";


@pure
fun dict.iDictSetRef(mutate self, keyLen: int, key: int, value: cell): void
    asm(value key self keyLen) "DICTISETREF";

@pure
fun dict.uDictSetRef(mutate self, keyLen: int, key: int, value: cell): void
    asm(value key self keyLen) "DICTUSETREF";

@pure
fun dict.sDictSetRef(mutate self, keyLen: int, key: slice, value: cell): void
    asm(value key self keyLen) "DICTSETREF";


@pure
fun dict.iDictSetIfNotExists(mutate self, keyLen: int, key: int, value: slice): bool
    asm(value key self keyLen) "DICTIADD";

@pure
fun dict.uDictSetIfNotExists(mutate self, keyLen: int, key: int, value: slice): bool
    asm(value key self keyLen) "DICTUADD";


@pure
fun dict.iDictSetIfExists(mutate self, keyLen: int, key: int, value: slice): bool
    asm(value key self keyLen) "DICTIREPLACE";

@pure
fun dict.uDictSetIfExists(mutate self, keyLen: int, key: int, value: slice): bool
    asm(value key self keyLen) "DICTUREPLACE";


@pure
fun dict.iDictGetRef(self, keyLen: int, key: int): (dict, bool)
    asm(key self keyLen) "DICTIGETREF" "NULLSWAPIFNOT";

@pure
fun dict.uDictGetRef(self, keyLen: int, key: int): (dict, bool)
    asm(key self keyLen) "DICTUGETREF" "NULLSWAPIFNOT";

@pure
fun dict.sDictGetRef(self, keyLen: int, key: slice): (dict, bool)
    asm(key self keyLen) "DICTGETREF" "NULLSWAPIFNOT";


@pure
fun dict.iDictGetRefOrNull(self, keyLen: int, key: int): dict
    asm(key self keyLen) "DICTIGETOPTREF";

@pure
fun dict.uDictGetRefOrNull(self, keyLen: int, key: int): dict
    asm(key self keyLen) "DICTUGETOPTREF";

@pure
fun dict.sDictGetRefOrNull(self, keyLen: int, key: slice): dict
    asm(key self keyLen) "DICTGETOPTREF";


@pure
fun dict.iDictDelete(mutate self, keyLen: int, key: int): bool
    asm(key self keyLen) "DICTIDEL";

@pure
fun dict.uDictDelete(mutate self, keyLen: int, key: int): bool
    asm(key self keyLen) "DICTUDEL";

@pure
fun dict.sDictDelete(mutate self, keyLen: int, key: slice): bool
    asm(key self keyLen) "DICTDEL";


@pure
fun dict.iDictSetAndGet(mutate self, keyLen: int, key: int, value: slice): (slice?, bool)
    asm(value key self keyLen) "DICTISETGET" "NULLSWAPIFNOT";

@pure
fun dict.uDictSetAndGet(mutate self, keyLen: int, key: int, value: slice): (slice?, bool)
    asm(value key self keyLen) "DICTUSETGET" "NULLSWAPIFNOT";

@pure
fun dict.sDictSetAndGet(mutate self, keyLen: int, key: slice, value: slice): (slice?, bool)
    asm(value key self keyLen) "DICTSETGET" "NULLSWAPIFNOT";


@pure
fun dict.iDictSetAndGetRefOrNull(mutate self, keyLen: int, key: int, value: cell): dict
    asm(value key self keyLen) "DICTISETGETOPTREF";

@pure
fun dict.uDictSetAndGetRefOrNull(mutate self, keyLen: int, key: int, value: cell): dict
    asm(value key self keyLen) "DICTUSETGETOPTREF";


@pure
fun dict.iDictDeleteAndGet(mutate self, keyLen: int, key: int): (slice?, bool)
    asm(key self keyLen) "DICTIDELGET" "NULLSWAPIFNOT";

@pure
fun dict.uDictDeleteAndGet(mutate self, keyLen: int, key: int): (slice?, bool)
    asm(key self keyLen) "DICTUDELGET" "NULLSWAPIFNOT";

@pure
fun dict.sDictDeleteAndGet(mutate self, keyLen: int, key: slice): (slice?, bool)
    asm(key self keyLen) "DICTDELGET" "NULLSWAPIFNOT";


@pure
fun dict.iDictSetBuilder(mutate self, keyLen: int, key: int, value: builder): void
    asm(value key self keyLen) "DICTISETB";

@pure
fun dict.uDictSetBuilder(mutate self, keyLen: int, key: int, value: builder): void
    asm(value key self keyLen) "DICTUSETB";

@pure
fun dict.sDictSetBuilder(mutate self, keyLen: int, key: slice, value: builder): void
    asm(value key self keyLen) "DICTSETB";


@pure
fun dict.iDictSetBuilderIfNotExists(mutate self, keyLen: int, key: int, value: builder): bool
    asm(value key self keyLen) "DICTIADDB";

@pure
fun dict.uDictSetBuilderIfNotExists(mutate self, keyLen: int, key: int, value: builder): bool
    asm(value key self keyLen) "DICTUADDB";

@pure
fun dict.iDictSetBuilderIfExists(mutate self, keyLen: int, key: int, value: builder): bool
    asm(value key self keyLen) "DICTIREPLACEB";

@pure
fun dict.uDictSetBuilderIfExists(mutate self, keyLen: int, key: int, value: builder): bool
    asm(value key self keyLen) "DICTUREPLACEB";


@pure
fun dict.iDictDeleteFirstAndGet(mutate self, keyLen: int): (int?, slice?, bool)
    asm(-> 0 2 1 3) "DICTIREMMIN" "NULLSWAPIFNOT2";

@pure
fun dict.uDictDeleteFirstAndGet(mutate self, keyLen: int): (int?, slice?, bool)
    asm(-> 0 2 1 3) "DICTUREMMIN" "NULLSWAPIFNOT2";

@pure
fun dict.sDictDeleteFirstAndGet(mutate self, keyLen: int): (slice?, slice?, bool)
    asm(-> 0 2 1 3) "DICTREMMIN" "NULLSWAPIFNOT2";


@pure
fun dict.iDictDeleteLastAndGet(mutate self, keyLen: int): (int?, slice?, bool)
    asm(-> 0 2 1 3) "DICTIREMMAX" "NULLSWAPIFNOT2";

@pure
fun dict.uDictDeleteLastAndGet(mutate self, keyLen: int): (int?, slice?, bool)
    asm(-> 0 2 1 3) "DICTUREMMAX" "NULLSWAPIFNOT2";

@pure
fun dict.sDictDeleteLastAndGet(mutate self, keyLen: int): (slice?, slice?, bool)
    asm(-> 0 2 1 3) "DICTREMMAX" "NULLSWAPIFNOT2";


@pure
fun dict.iDictGetFirst(self, keyLen: int): (int?, slice?, bool)
    asm (-> 1 0 2) "DICTIMIN" "NULLSWAPIFNOT2";

@pure
fun dict.uDictGetFirst(self, keyLen: int): (int?, slice?, bool)
    asm (-> 1 0 2) "DICTUMIN" "NULLSWAPIFNOT2";

@pure
fun dict.sDictGetFirst(self, keyLen: int): (slice?, slice?, bool)
    asm (-> 1 0 2) "DICTMIN" "NULLSWAPIFNOT2";

@pure
fun dict.iDictGetFirstAsRef(self, keyLen: int): (int?, dict, bool)
    asm (-> 1 0 2) "DICTIMINREF" "NULLSWAPIFNOT2";

@pure
fun dict.uDictGetFirstAsRef(self, keyLen: int): (int?, dict, bool)
    asm (-> 1 0 2) "DICTUMINREF" "NULLSWAPIFNOT2";

@pure
fun dict.sDictGetFirstAsRef(self, keyLen: int): (slice?, dict, bool)
    asm (-> 1 0 2) "DICTMINREF" "NULLSWAPIFNOT2";


@pure
fun dict.iDictGetLast(self, keyLen: int): (int?, slice?, bool)
    asm (-> 1 0 2) "DICTIMAX" "NULLSWAPIFNOT2";

@pure
fun dict.uDictGetLast(self, keyLen: int): (int?, slice?, bool)
    asm (-> 1 0 2) "DICTUMAX" "NULLSWAPIFNOT2";

@pure
fun dict.sDictGetLast(self, keyLen: int): (slice?, slice?, bool)
    asm (-> 1 0 2) "DICTMAX" "NULLSWAPIFNOT2";

@pure
fun dict.iDictGetLastAsRef(self, keyLen: int): (int?, dict, bool)
    asm (-> 1 0 2) "DICTIMAXREF" "NULLSWAPIFNOT2";

@pure
fun dict.uDictGetLastAsRef(self, keyLen: int): (int?, dict, bool)
    asm (-> 1 0 2) "DICTUMAXREF" "NULLSWAPIFNOT2";

@pure
fun dict.sDictGetLastAsRef(self, keyLen: int): (slice?, dict, bool)
    asm (-> 1 0 2) "DICTMAXREF" "NULLSWAPIFNOT2";


@pure
fun dict.iDictGetNext(self, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETNEXT" "NULLSWAPIFNOT2";

@pure
fun dict.uDictGetNext(self, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETNEXT" "NULLSWAPIFNOT2";

@pure
fun dict.iDictGetNextOrEqual(self, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETNEXTEQ" "NULLSWAPIFNOT2";

@pure
fun dict.uDictGetNextOrEqual(self, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETNEXTEQ" "NULLSWAPIFNOT2";


@pure
fun dict.iDictGetPrev(self, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETPREV" "NULLSWAPIFNOT2";

@pure
fun dict.uDictGetPrev(self, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETPREV" "NULLSWAPIFNOT2";

@pure
fun dict.iDictGetPrevOrEqual(self, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETPREVEQ" "NULLSWAPIFNOT2";

@pure
fun dict.uDictGetPrevOrEqual(self, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETPREVEQ" "NULLSWAPIFNOT2";


/**
 Prefix dictionary primitives.
 */

@pure
fun dict.prefixDictGet(self, keyLen: int, key: slice): (slice, slice?, slice?, bool)
    asm(key self keyLen) "PFXDICTGETQ" "NULLSWAPIFNOT2";

@pure
fun dict.prefixDictSet(mutate self, keyLen: int, key: slice, value: slice): bool
    asm(value key self keyLen) "PFXDICTSET";

@pure
fun dict.prefixDictDelete(mutate self, keyLen: int, key: slice): bool
    asm(key self keyLen) "PFXDICTDEL";
