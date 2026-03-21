// A part of standard library for Tolk
tolk 1.3

/**
    A string is a language primitive — a cell containing snake-formatted text data.
    Short strings are (typically) represented as a cell, like "abcd".
    Since the limit of a cell is 1023 bits (127 bytes), the only way to represent longer strings
  is to use snake format: "start" + (ref "middle" + (ref "end")).
    Short strings can also be stored in snake format, e.g. "ab" + (ref "cd").
    The outer world, knowing that a particular cell is a string, can iteratively concatenate
  all references and render a string in UI.
 */

/// Returns snake depth of a string.
/// Example: "abcd" has depth 0, it's a flat string.
/// Example: "ab" + (ref "cd") has depth 1.
/// Example: "a" + (ref "bc" + (ref "d" + (ref ""))) has depth 3.
@pure
fun string.depth(self): int
    asm "CDEPTH"

/// Calculates length of a string in bytes (not unicode symbols, just bytes).
/// Since strings are stored in snake format (chain of cells), this requires traversing all cells.
@pure
@inline_ref
fun string.calculateLength(self): int {
    var bits = 0;
    var snake = self.beginParse();
    repeat (snake.depth()) {
        bits += snake.remainingBitsCount();
        snake = snake.loadRef().beginParse();
    }
    bits += snake.remainingBitsCount();
    return bits / 8;
}

/// Computes a content hash of a string, traversing all snake cells.
/// The hash is the same regardless of snake layout:
/// "abcd", "ab" + (ref "cd"), "a" + (ref "bcd" + (ref "")) all produce the same hash.
/// Uses FNV-1a hashing with standard 128-bit parameters.
@pure
@inline_ref
fun string.hash(self): uint256 {
    val FNV_PRIME = 309485009821345068724781371;
    val MASK = (1 << 128) - 1;
    var snake = self.beginParse();
    var hash = 144066263297769815596495629667062367629;
    repeat (snake.depth()) {
        repeat (snake.remainingBitsCount() / 8) {
            hash = ((hash ^ snake.loadUint(8)) * FNV_PRIME) & MASK;
        }
        snake = snake.loadRef().beginParse();
    }
    repeat (snake.remainingBitsCount() / 8) {
        hash = ((hash ^ snake.loadUint(8)) * FNV_PRIME) & MASK;
    }
    return hash;
}

/// Compares two strings, considering snake cells.
/// For example, "abcd" is equal to "ab" + (ref "cd") is equal to "a" + (ref "bc" + (ref "d")).
@pure
fun string.equalTo(self, another: string): bool {
    return self.depth() + another.depth()
        // some is snake, so we have "ab->cd" / "a->bc->d" / etc., do full traverse
        ? self.hash() == another.hash()
        // fast path for flat strings (no refs), use native cell hashes
        : (self as unknown as cell).hash() == (another as unknown as cell).hash()
}

/// Converts an integer to its decimal string representation.
/// Returns a string (a TVM cell on a stack).
fun int.toDecimalString(self): string asm """
    NEWC
    SWAP                      // b x
    DUP                       // b x x
    ISNEG
    IF:<{                     // if x<0
      SWAP                    // x b
      x{2D} STSLICECONST      // store '-' = 45 = 0x2D
      SWAP                    // b x
      NEGATE                  // b |x|
    }>
    // now: b x (x is always >= 0)
    0 PUSHINT                 // b x i=0
    SWAP                      // b i=0 x
    UNTIL:<{                  // loop: extract digits from x, push them under builder
      10 PUSHINT DIVMOD       // b i x/10 x%10
      48 ADDCONST             // b i x digit (digit = '0' + remainder)
      s3 s1 s3 XCHG3          // digit b x i (digit goes under builder)
      INC                     // digit b x i+1
      s1 s0 XCPU              // digit b i x x (copy x to check if zero)
      ISZERO                  // exit when x == 0
    }>
    DROP                      // remove x (now 0)
    // stack: d1 d2 ... dN b i (digits in correct order, oldest at bottom)
    REPEAT:<{ 8 STU }>        // store i digits (8 bits each) into builder
    ENDC                      // finalize cell
"""

/// A utility for building strings at runtime by appending multiple parts.
/// The result is a valid snake-format string.
/// ```
/// var s = StringBuilder.create().append("Hello, ").append(userName).append("!").build();
/// ```
/// After this, `s` will be a chain of cells: "Hello, " + (ref userName + (ref "!"))
struct StringBuilder {
    private chunks: array<string> = []
}

/// Creates an empty `StringBuilder`.
/// ```
/// var s = StringBuilder.create().append(...).build();
/// ```
fun StringBuilder.create(): StringBuilder {
    return {}
}

/// Appends a string to the builder. The string may itself be a snake chain.
fun StringBuilder.append(mutate self, s: string): self {
    self.chunks.push(s);
    return self;
}

/// Appends an integer as its decimal representation.
/// For example, `sb.appendInt(-42)` appends "-42".
@inline_ref
fun StringBuilder.appendInt(mutate self, n: int): self {
    self.chunks.push(n.toDecimalString());
    return self;
}

/// Builds the final string by concatenating all appended parts into a snake chain.
/// The result is a valid snake-format string where each cell contains data and optionally a ref to next.
/// Throws an exception if `append()` was not called (a builder is empty).
@inline_ref
fun StringBuilder.build(self): string {
    // the last string, as any other, is probably "xyz", probably snaked "x->y->z"
    // we'll leave it as-is (TVM cell), won't even call `beginParse` for it
    var tail = self.chunks.last() as unknown as cell;
    var idx = self.chunks.size() - 1;
    // we can have simple chunks [ "ab", "cd", "xy" ], it's the hot path, loop backwards
    // but some middle strings might be snaked: [ "ab->c->d", "xy->z" ], we have to unwrap them
    repeat (idx) {
        idx -= 1;
        var chunk = self.chunks.get(idx).beginParse();
        // in case the middle string is a snake: "ab->c->d" + tail
        if (chunk.depth()) {
            // split "ab->c->d" into [ "ab", "c", "d" ]
            var snake = chunk;
            var subBits = array<slice> [];
            repeat (snake.depth()) {
                var nextSnake = snake.loadRef().beginParse();
                subBits.push(snake);
                snake = nextSnake;
            }
            subBits.push(snake);
            // calculate { tail = "d" + tail; tail = "c" + tail }
            var lastIdx = subBits.size() - 1;
            repeat (lastIdx) {
                tail = (subBits.get(lastIdx), tail).toCell();
                lastIdx -= 1;
            }
            // save chunk = "ab", below it will be concatenated "ab" + tail
            chunk = subBits.first();
        }
        // tail = "cur_str" + tail
        tail = (chunk, tail).toCell();
    }
    // return a TVM cell, which is a valid snake string
    return tail as unknown as string;
}

/// A string, which contents starts with 0x00/0x01 byte.
/// By standard, offchain explorers/APIs may treat contents of a string based on 0xXX prefixes:
/// - 0x00 + "str" — means "this is a regular snake-encoded string"
/// - 0x01 + "str" — means "this is an off-chain content URL"
/// Used in jettons and NFTs for responses from GET methods to comply with TEPs (token standards).
type string_prefixed0x = string

/// Given "str", prefix it with 0x00 byte.
/// Logically it makes a string "\x00str"; physically, it's 0x00 + ref "str".
/// See [string_prefixed0x].
fun string.prefixWith00(self): string_prefixed0x {
    val cell00 = beginCell().storeUint(0x00, 8).storeString(self).endCell();
    return cell00 as unknown as string;
}

/// Given "str", prefix it with 0x01 byte.
/// Logically it makes a string "\x01str"; physically, it's 0x01 + ref "str".
/// See [string_prefixed0x].
fun string.prefixWith01(self): string_prefixed0x {
    val cell01 = beginCell().storeUint(0x01, 8).storeString(self).endCell();
    return cell01 as unknown as string;
}
