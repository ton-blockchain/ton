fun main(s: string) {
    return s.crc32();
}

/**
@compilation_should_fail
@stderr method `crc32` requires a constant string, like "some_str".crc32()
 */
