// line1
/* */ import "unexisting.tolk";
// line3

/**
@compilation_should_fail
@stderr invalid-import.tolk:2:7: error: Failed to import: cannot find file
@stderr import "unexisting.tolk";
 */
