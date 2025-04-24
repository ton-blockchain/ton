// A part of standard library for Tolk
tolk 0.11

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
fun dictIsEmpty(self: dict): bool
    asm "DICTEMPTY";


@pure
fun iDictGet(self: dict, keyLen: int, key: int): (slice?, bool)
    asm(key self keyLen) "DICTIGET" "NULLSWAPIFNOT";

@pure
fun uDictGet(self: dict, keyLen: int, key: int): (slice?, bool)
    asm(key self keyLen) "DICTUGET" "NULLSWAPIFNOT";

@pure
fun sDictGet(self: dict, keyLen: int, key: slice): (slice?, bool)
    asm(key self keyLen) "DICTGET" "NULLSWAPIFNOT";


@pure
fun iDictSet(mutate self: dict, keyLen: int, key: int, value: slice): void
    asm(value key self keyLen) "DICTISET";

@pure
fun uDictSet(mutate self: dict, keyLen: int, key: int, value: slice): void
    asm(value key self keyLen) "DICTUSET";

@pure
fun sDictSet(mutate self: dict, keyLen: int, key: slice, value: slice): void
    asm(value key self keyLen) "DICTSET";


@pure
fun iDictSetRef(mutate self: dict, keyLen: int, key: int, value: cell): void
    asm(value key self keyLen) "DICTISETREF";

@pure
fun uDictSetRef(mutate self: dict, keyLen: int, key: int, value: cell): void
    asm(value key self keyLen) "DICTUSETREF";

@pure
fun sDictSetRef(mutate self: dict, keyLen: int, key: slice, value: cell): void
    asm(value key self keyLen) "DICTSETREF";


@pure
fun iDictSetIfNotExists(mutate self: dict, keyLen: int, key: int, value: slice): bool
    asm(value key self keyLen) "DICTIADD";

@pure
fun uDictSetIfNotExists(mutate self: dict, keyLen: int, key: int, value: slice): bool
    asm(value key self keyLen) "DICTUADD";


@pure
fun iDictSetIfExists(mutate self: dict, keyLen: int, key: int, value: slice): bool
    asm(value key self keyLen) "DICTIREPLACE";

@pure
fun uDictSetIfExists(mutate self: dict, keyLen: int, key: int, value: slice): bool
    asm(value key self keyLen) "DICTUREPLACE";


@pure
fun iDictGetRef(self: dict, keyLen: int, key: int): (dict, bool)
    asm(key self keyLen) "DICTIGETREF" "NULLSWAPIFNOT";

@pure
fun uDictGetRef(self: dict, keyLen: int, key: int): (dict, bool)
    asm(key self keyLen) "DICTUGETREF" "NULLSWAPIFNOT";

@pure
fun sDictGetRef(self: dict, keyLen: int, key: slice): (dict, bool)
    asm(key self keyLen) "DICTGETREF" "NULLSWAPIFNOT";


@pure
fun iDictGetRefOrNull(self: dict, keyLen: int, key: int): dict
    asm(key self keyLen) "DICTIGETOPTREF";

@pure
fun uDictGetRefOrNull(self: dict, keyLen: int, key: int): dict
    asm(key self keyLen) "DICTUGETOPTREF";

@pure
fun sDictGetRefOrNull(self: dict, keyLen: int, key: slice): dict
    asm(key self keyLen) "DICTGETOPTREF";


@pure
fun iDictDelete(mutate self: dict, keyLen: int, key: int): bool
    asm(key self keyLen) "DICTIDEL";

@pure
fun uDictDelete(mutate self: dict, keyLen: int, key: int): bool
    asm(key self keyLen) "DICTUDEL";

@pure
fun sDictDelete(mutate self: dict, keyLen: int, key: slice): bool
    asm(key self keyLen) "DICTDEL";


@pure
fun iDictSetAndGet(mutate self: dict, keyLen: int, key: int, value: slice): (slice?, bool)
    asm(value key self keyLen) "DICTISETGET" "NULLSWAPIFNOT";

@pure
fun uDictSetAndGet(mutate self: dict, keyLen: int, key: int, value: slice): (slice?, bool)
    asm(value key self keyLen) "DICTUSETGET" "NULLSWAPIFNOT";

@pure
fun sDictSetAndGet(mutate self: dict, keyLen: int, key: slice, value: slice): (slice?, bool)
    asm(value key self keyLen) "DICTSETGET" "NULLSWAPIFNOT";


@pure
fun iDictSetAndGetRefOrNull(mutate self: dict, keyLen: int, key: int, value: cell): dict
    asm(value key self keyLen) "DICTISETGETOPTREF";

@pure
fun uDictSetAndGetRefOrNull(mutate self: dict, keyLen: int, key: int, value: cell): dict
    asm(value key self keyLen) "DICTUSETGETOPTREF";


@pure
fun iDictDeleteAndGet(mutate self: dict, keyLen: int, key: int): (slice?, bool)
    asm(key self keyLen) "DICTIDELGET" "NULLSWAPIFNOT";

@pure
fun uDictDeleteAndGet(mutate self: dict, keyLen: int, key: int): (slice?, bool)
    asm(key self keyLen) "DICTUDELGET" "NULLSWAPIFNOT";

@pure
fun sDictDeleteAndGet(mutate self: dict, keyLen: int, key: slice): (slice?, bool)
    asm(key self keyLen) "DICTDELGET" "NULLSWAPIFNOT";


@pure
fun iDictSetBuilder(mutate self: dict, keyLen: int, key: int, value: builder): void
    asm(value key self keyLen) "DICTISETB";

@pure
fun uDictSetBuilder(mutate self: dict, keyLen: int, key: int, value: builder): void
    asm(value key self keyLen) "DICTUSETB";

@pure
fun sDictSetBuilder(mutate self: dict, keyLen: int, key: slice, value: builder): void
    asm(value key self keyLen) "DICTSETB";


@pure
fun iDictSetBuilderIfNotExists(mutate self: dict, keyLen: int, key: int, value: builder): bool
    asm(value key self keyLen) "DICTIADDB";

@pure
fun uDictSetBuilderIfNotExists(mutate self: dict, keyLen: int, key: int, value: builder): bool
    asm(value key self keyLen) "DICTUADDB";

@pure
fun iDictSetBuilderIfExists(mutate self: dict, keyLen: int, key: int, value: builder): bool
    asm(value key self keyLen) "DICTIREPLACEB";

@pure
fun uDictSetBuilderIfExists(mutate self: dict, keyLen: int, key: int, value: builder): bool
    asm(value key self keyLen) "DICTUREPLACEB";


@pure
fun iDictDeleteFirstAndGet(mutate self: dict, keyLen: int): (int?, slice?, bool)
    asm(-> 0 2 1 3) "DICTIREMMIN" "NULLSWAPIFNOT2";

@pure
fun uDictDeleteFirstAndGet(mutate self: dict, keyLen: int): (int?, slice?, bool)
    asm(-> 0 2 1 3) "DICTUREMMIN" "NULLSWAPIFNOT2";

@pure
fun sDictDeleteFirstAndGet(mutate self: dict, keyLen: int): (slice?, slice?, bool)
    asm(-> 0 2 1 3) "DICTREMMIN" "NULLSWAPIFNOT2";


@pure
fun iDictDeleteLastAndGet(mutate self: dict, keyLen: int): (int?, slice?, bool)
    asm(-> 0 2 1 3) "DICTIREMMAX" "NULLSWAPIFNOT2";

@pure
fun uDictDeleteLastAndGet(mutate self: dict, keyLen: int): (int?, slice?, bool)
    asm(-> 0 2 1 3) "DICTUREMMAX" "NULLSWAPIFNOT2";

@pure
fun sDictDeleteLastAndGet(mutate self: dict, keyLen: int): (slice?, slice?, bool)
    asm(-> 0 2 1 3) "DICTREMMAX" "NULLSWAPIFNOT2";


@pure
fun iDictGetFirst(self: dict, keyLen: int): (int?, slice?, bool)
    asm (-> 1 0 2) "DICTIMIN" "NULLSWAPIFNOT2";

@pure
fun uDictGetFirst(self: dict, keyLen: int): (int?, slice?, bool)
    asm (-> 1 0 2) "DICTUMIN" "NULLSWAPIFNOT2";

@pure
fun sDictGetFirst(self: dict, keyLen: int): (slice?, slice?, bool)
    asm (-> 1 0 2) "DICTMIN" "NULLSWAPIFNOT2";

@pure
fun iDictGetFirstAsRef(self: dict, keyLen: int): (int?, dict, bool)
    asm (-> 1 0 2) "DICTIMINREF" "NULLSWAPIFNOT2";

@pure
fun uDictGetFirstAsRef(self: dict, keyLen: int): (int?, dict, bool)
    asm (-> 1 0 2) "DICTUMINREF" "NULLSWAPIFNOT2";

@pure
fun sDictGetFirstAsRef(self: dict, keyLen: int): (slice?, dict, bool)
    asm (-> 1 0 2) "DICTMINREF" "NULLSWAPIFNOT2";


@pure
fun iDictGetLast(self: dict, keyLen: int): (int?, slice?, bool)
    asm (-> 1 0 2) "DICTIMAX" "NULLSWAPIFNOT2";

@pure
fun uDictGetLast(self: dict, keyLen: int): (int?, slice?, bool)
    asm (-> 1 0 2) "DICTUMAX" "NULLSWAPIFNOT2";

@pure
fun sDictGetLast(self: dict, keyLen: int): (slice?, slice?, bool)
    asm (-> 1 0 2) "DICTMAX" "NULLSWAPIFNOT2";

@pure
fun iDictGetLastAsRef(self: dict, keyLen: int): (int?, dict, bool)
    asm (-> 1 0 2) "DICTIMAXREF" "NULLSWAPIFNOT2";

@pure
fun uDictGetLastAsRef(self: dict, keyLen: int): (int?, dict, bool)
    asm (-> 1 0 2) "DICTUMAXREF" "NULLSWAPIFNOT2";

@pure
fun sDictGetLastAsRef(self: dict, keyLen: int): (slice?, dict, bool)
    asm (-> 1 0 2) "DICTMAXREF" "NULLSWAPIFNOT2";


@pure
fun iDictGetNext(self: dict, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETNEXT" "NULLSWAPIFNOT2";

@pure
fun uDictGetNext(self: dict, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETNEXT" "NULLSWAPIFNOT2";

@pure
fun iDictGetNextOrEqual(self: dict, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETNEXTEQ" "NULLSWAPIFNOT2";

@pure
fun uDictGetNextOrEqual(self: dict, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETNEXTEQ" "NULLSWAPIFNOT2";


@pure
fun iDictGetPrev(self: dict, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETPREV" "NULLSWAPIFNOT2";

@pure
fun uDictGetPrev(self: dict, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETPREV" "NULLSWAPIFNOT2";

@pure
fun iDictGetPrevOrEqual(self: dict, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTIGETPREVEQ" "NULLSWAPIFNOT2";

@pure
fun uDictGetPrevOrEqual(self: dict, keyLen: int, pivot: int): (int?, slice?, bool)
    asm(pivot self keyLen -> 1 0 2) "DICTUGETPREVEQ" "NULLSWAPIFNOT2";


/**
 Prefix dictionary primitives.
 */

@pure
fun prefixDictGet(self: dict, keyLen: int, key: slice): (slice, slice?, slice?, bool)
    asm(key self keyLen) "PFXDICTGETQ" "NULLSWAPIFNOT2";

@pure
fun prefixDictSet(mutate self: dict, keyLen: int, key: slice, value: slice): bool
    asm(value key self keyLen) "PFXDICTSET";

@pure
fun prefixDictDelete(mutate self: dict, keyLen: int, key: slice): bool
    asm(key self keyLen) "PFXDICTDEL";
