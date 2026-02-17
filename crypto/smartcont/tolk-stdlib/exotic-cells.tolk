// A part of standard library for Tolk
tolk 1.2

enum ExoticCellType: int8 {
    Ordinary = -1
    PrunedBranch = 1
    LibraryReference = 2
    MerkleProof = 3
    MerkleUpdate = 4
}

/// Transforms an ordinary or exotic cell into a slice, as if it were an ordinary cell. 
/// A flag is returned indicating whether c is exotic. 
/// If so, ExoticCellType can later be deserialized from the first eight bits of s.
/// ```
/// var (s, isExotic) = c.beginParseSpecial();
/// if (isExotic) {
///     // use unsafe cast because the first 8 bits hold a correct value, no need for validation
///     val cellType = s.loadInt(8) as ExoticCellType;
/// }
/// ```
@pure
fun cell.beginParseSpecial(self): (slice, bool)
    asm "XCTOS"

/// Creates an exotic or ordinary cell from a builder.
@pure
fun builder.endCellSpecial(self, isExotic: bool = true): cell
    asm "ENDXC"

/// Returns i-th hash of the cell.
@pure
fun cell.hashOfLevel(self, level: int): uint256
    asm "CHASHIX"

/// Returns i-th depth of the cell.
@pure
fun cell.depthOfLevel(self, level: int): int
    asm "CDEPTHIX"

/// Loads an exotic cell and returns an ordinary cell.
/// If the cell is already ordinary, does nothing. Throws an exception if cannot be loaded.
@pure
fun cell.loadSpecial(self): cell
    asm "XLOAD"

/// Transforms an ordinary or exotic cell into a library reference
/// (into an exotic cell with contents "8 bits cell type" + "256 bits lib hash").
@pure
fun cell.toLibraryReference(self): cell {
    return beginCell()
        .storeInt(ExoticCellType.LibraryReference as int, 8)
        .storeUint(self.hash(), 256)
        .endCellSpecial()
}
