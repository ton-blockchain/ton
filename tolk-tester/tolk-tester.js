// Usage: `node tolk-tester.js tests_dir` OR `node tolk-tester.js tests_dir file_pattern`
// from current dir, providing some env (see getenv() calls).
// This is a JS version of tolk-tester.py to test Tolk compiled to WASM.
// Don't forget to keep it identical to Python version!

const fs = require('fs');
const os = require('os');
const path = require('path');
const child_process = require('child_process');

function print(...args) {
    console.log(...args)
}

/** @return {string} */
function getenv(name, def = null) {
    if (name in process.env)
        return process.env[name]
    if (def === null) {
        print(`Environment variable ${name} is not set`)
        process.exit(1)
    }
    return def
}

const TOLKFIFTLIB_MODULE = getenv('TOLKFIFTLIB_MODULE')
const TOLKFIFTLIB_WASM = getenv('TOLKFIFTLIB_WASM')
const FIFT_EXECUTABLE = getenv('FIFT_EXECUTABLE')
const FIFT_LIBS_FOLDER = getenv('FIFTPATH')  // this env is needed for fift to work properly
const STDLIB_FOLDER = __dirname + '/../crypto/smartcont/tolk-stdlib'
const TMP_DIR = os.tmpdir()

class CmdLineOptions {
    constructor(/**string[]*/ argv) {
        if (argv.length < 3) {
            print("Usage: node tolk-tester.js tests_dir [file_pattern]")
            process.exit(1)
        }
        if (!fs.existsSync(argv[2]) || !fs.lstatSync(argv[2]).isDirectory()) {
            print(`Directory '${argv[2]}' doesn't exist`)
            process.exit(1)
        }

        this.tests_dir = argv[2]
        this.file_pattern = argv[3]
    }

    /** @return {string[]} */
    find_tests() {
        let all_test_files = []
        let all_children_of_tests_dir = fs.readdirSync(this.tests_dir)
        all_children_of_tests_dir.sort()
        for (let f of all_children_of_tests_dir)
            if (f.endsWith(".tolk"))
                all_test_files.push(path.join(this.tests_dir, f))
        for (let f of all_children_of_tests_dir)
            if (!f.endsWith(".tolk") && f !== "imports") {
                let subdir = path.join(this.tests_dir, f)
                let children_of_subdir = fs.readdirSync(subdir)
                children_of_subdir.sort()
                all_test_files.push(...children_of_subdir.map(f => path.join(subdir, f)))
            }

        if (this.file_pattern)
          all_test_files = all_test_files.filter(f => f.includes(this.file_pattern))
        return all_test_files
    }
}


class ParseInputError extends Error {
}

class TolkCompilationFailedError extends Error {
    constructor(/**string*/ message, /**string*/ stderr) {
        super(message);
        this.stderr = stderr
    }
}

class TolkCompilationSucceededError extends Error {
}

class FiftExecutionFailedError extends Error {
    constructor(/**string*/ message, /**string*/ stderr) {
        super(message);
        this.stderr = stderr
    }
}

class CompareOutputError extends Error {
    constructor(/**string*/ message, /**string*/ output) {
        super(message);
        this.output = output
    }
}

class CompareFifCodegenError extends Error {
}

class CompareCodeHashError extends Error {
}


/*
 * In positive tests, there are several testcases "input X should produce output Y".
 */
class TolkTestCaseInputOutput {
    static reJustNumber = /^[-+]?\d+$/
    static reMathExpr = /^[0x123456789()+\-*/<>]*$/

    constructor(/**string*/ method_id_str, /**string*/ input_str, /**string*/ output_str) {
        let processed_inputs = []
        for (let in_arg of input_str.split(' ')) {
            if (in_arg.length === 0)
                continue
            else if (in_arg.startsWith("x{") || TolkTestCaseInputOutput.reJustNumber.test(in_arg))
                processed_inputs.push(in_arg)
            else if (TolkTestCaseInputOutput.reMathExpr.test(in_arg))
                // replace "3<<254" with "3n<<254n" (big number) before eval (in Python we don't need this)
                processed_inputs.push(eval(in_arg.replace('//', '/').replace(/(\d)($|\D)/gmi, '$1n$2')).toString())
            else if (in_arg === "null")
                processed_inputs.push("null")
            else
                throw new ParseInputError(`'${in_arg}' can't be evaluated`)
        }

        this.method_id = +method_id_str
        this.input = processed_inputs.join(' ')
        this.expected_output = output_str
    }

    check(/**string[]*/ stdout_lines, /**number*/ line_idx) {
        if (stdout_lines[line_idx] !== this.expected_output)
            throw new CompareOutputError(`error on case #${line_idx + 1} (${this.method_id} | ${this.input}):\n    expect: ${this.expected_output}\n    actual: ${stdout_lines[line_idx]}`, stdout_lines.join("\n"))
    }
}

/*
 * @stderr checks, when compilation fails, that stderr (compilation error) is expected.
 * If it's multiline, all lines must be present in specified order.
 */
class TolkTestCaseStderr {
    constructor(/**string[]*/ stderr_pattern, /**boolean*/ avoid) {
        this.stderr_pattern = stderr_pattern
        this.avoid = avoid
    }

    check(/**string*/ stderr) {
        const line_match = this.find_pattern_in_stderr(stderr.split(/\n/))
        if (line_match === -1 && !this.avoid)
            throw new CompareOutputError("pattern not found in stderr:\n" +
                this.stderr_pattern.map(x => "    " + x).join("\n"), stderr)
        else if (line_match !== -1 && this.avoid)
            throw new CompareOutputError(`pattern found (line ${line_match + 1}), but not expected to be:\n` +
                this.stderr_pattern.map(x => "    " + x).join("\n"), stderr)
    }

    find_pattern_in_stderr(/**string[]*/ stderr) {
        for (let line_start = 0; line_start < stderr.length; ++line_start)
            if (this.try_match_pattern(0, stderr, line_start))
                return line_start
        return -1
    }

    try_match_pattern(/**number*/ pattern_offset, /**string[]*/ stderr, /**number*/ offset) {
        if (pattern_offset >= this.stderr_pattern.length)
            return true
        if (offset >= stderr.length)
            return false

        const line_pattern = this.stderr_pattern[pattern_offset]
        const line_output = stderr[offset]
        return line_output.includes(line_pattern) && this.try_match_pattern(pattern_offset + 1, stderr, offset + 1)
    }
}

/*
 * @fif_codegen checks that contents of compiled.fif matches the expected pattern.
 * @fif_codegen_avoid checks that is does not match the pattern.
 * See comments in run_tests.py.
 */
class TolkTestCaseFifCodegen {
    constructor(/**string[]*/ fif_pattern, /**boolean*/ avoid) {
        /** @type {string[]} */
        this.fif_pattern = fif_pattern.map(s => s.trim())
        this.avoid = avoid
    }

    check(/**string[]*/ fif_output) {
        const line_match = this.find_pattern_in_fif_output(fif_output)
        if (line_match === -1 && !this.avoid)
            throw new CompareFifCodegenError("pattern not found:\n" +
                this.fif_pattern.map(x => "    " + x).join("\n"))
        else if (line_match !== -1 && this.avoid)
            throw new CompareFifCodegenError(`pattern found (line ${line_match + 1}), but not expected to be:\n` +
                this.fif_pattern.map(x => "    " + x).join("\n"))
    }

    find_pattern_in_fif_output(/**string[]*/ fif_output) {
        for (let line_start = 0; line_start < fif_output.length; ++line_start)
            if (this.try_match_pattern(0, fif_output, line_start))
                return line_start
        return -1
    }

    try_match_pattern(/**number*/ pattern_offset, /**string[]*/ fif_output, /**number*/ offset) {
        if (pattern_offset >= this.fif_pattern.length)
            return true
        if (offset >= fif_output.length)
            return false
        const line_pattern = this.fif_pattern[pattern_offset]
        const line_output = fif_output[offset]

        if (line_pattern !== "...") {
            if (!TolkTestCaseFifCodegen.does_line_match(line_pattern, line_output))
                return false
            return this.try_match_pattern(pattern_offset + 1, fif_output, offset + 1)
        }
        while (offset < fif_output.length) {
            if (this.try_match_pattern(pattern_offset + 1, fif_output, offset))
                return true
            offset = offset + 1
        }
        return false
    }

    static split_line_to_cmd_and_comment(/**string*/ trimmed_line) {
        const pos = trimmed_line.indexOf("//")
        if (pos === -1)
            return [trimmed_line, null]
        else
            return [trimmed_line.substring(0, pos).trimEnd(), trimmed_line.substring(pos + 2).trimStart()]
    }

    static does_line_match(/**string*/ line_pattern, /**string*/ line_output) {
        const [cmd_pattern, comment_pattern] = TolkTestCaseFifCodegen.split_line_to_cmd_and_comment(line_pattern)
        const [cmd_output, comment_output] = TolkTestCaseFifCodegen.split_line_to_cmd_and_comment(line_output.trim())
        return cmd_pattern === cmd_output && (comment_pattern === null || comment_pattern === comment_output)
    }
}

/*
 * @code_hash checks that hash of compiled output.fif matches the provided value.
 * It's used to "record" code boc hash and to check that it remains the same on compiler modifications.
 * Being much less flexible than @fif_codegen, it nevertheless gives a guarantee of bytecode stability.
 */
class TolkTestCaseExpectedHash {
    constructor(/**string*/ expected_hash) {
        this.code_hash = expected_hash
    }

    check(/**string*/ fif_code_hash) {
        if (this.code_hash !== fif_code_hash)
            throw new CompareCodeHashError(`expected ${this.code_hash}, actual ${fif_code_hash}`)
    }
}


class TolkTestFile {
    constructor(/**string*/ tolk_filename, /**string*/ artifacts_folder) {
        this.line_idx = 0
        this.tolk_filename = tolk_filename
        this.artifacts_folder = artifacts_folder
        this.compilation_should_fail = false
        /** @type {TolkTestCaseStderr[]} */
        this.stderr_includes = []
        /** @type {TolkTestCaseInputOutput[]} */
        this.input_output = []
        /** @type {TolkTestCaseFifCodegen[]} */
        this.fif_codegen = []
        /** @type {TolkTestCaseExpectedHash | null} */
        this.expected_hash = null
        /** @type {string | null} */
        this.experimental_options = null
        /** @type {boolean} */
        this.enable_tolk_lines_comments = false
    }

    parse_input_from_tolk_file() {
        const lines = fs.readFileSync(this.tolk_filename, 'utf-8').split(/\r?\n/)
        this.line_idx = 0

        while (this.line_idx < lines.length) {
            const line = lines[this.line_idx]
            if (line.startsWith('@testcase')) {
                let s = line.split("|").map(p => p.trim())
                if (s.length !== 4)
                    throw new ParseInputError(`incorrect format of @testcase: ${line}`)
                this.input_output.push(new TolkTestCaseInputOutput(s[1], s[2], s[3]))
            } else if (line.startsWith('@compilation_should_fail')) {
                this.compilation_should_fail = true
            } else if (line.startsWith('@stderr')) {
                this.stderr_includes.push(new TolkTestCaseStderr(this.parse_string_value(lines), false))
            } else if (line.startsWith("@fif_codegen_avoid")) {
                this.fif_codegen.push(new TolkTestCaseFifCodegen(this.parse_string_value(lines), true))
            } else if (line.startsWith("@fif_codegen_enable_comments")) {
                this.enable_tolk_lines_comments = true
            } else if (line.startsWith("@fif_codegen")) {
                this.fif_codegen.push(new TolkTestCaseFifCodegen(this.parse_string_value(lines), false))
            } else if (line.startsWith("@code_hash")) {
                this.expected_hash = new TolkTestCaseExpectedHash(this.parse_string_value(lines, false)[0])
            } else if (line.startsWith("@experimental_options")) {
                this.experimental_options = line.substring(22)
            }
            this.line_idx++
        }

        if (this.input_output.length === 0 && !this.compilation_should_fail)
            throw new ParseInputError("no @testcase present")
        if (this.input_output.length !== 0 && this.compilation_should_fail)
            throw new ParseInputError("@testcase present, but compilation_should_fail")
    }

    /** @return {string[]} */
    parse_string_value(/**string[]*/ lines, allow_multiline = true) {
        // a tag must be followed by a space (single-line), e.g. '@stderr some text'
        // or be a multi-line value, surrounded by """
        const line = lines[this.line_idx]
        const pos_sp = line.indexOf(' ')
        const is_multi_line = lines[this.line_idx + 1] === '"""'
        const is_single_line = pos_sp !== -1
        if (!is_single_line && !is_multi_line)
            throw new ParseInputError(`${line} value is empty (not followed by a string or a multiline """)`)
        if (is_single_line && is_multi_line)
            throw new ParseInputError(`${line.substring(0, pos_sp)} value is both single-line and followed by """`)
        if (is_multi_line && !allow_multiline)
            throw new ParseInputError(`${line} value should be single-line`);

        if (is_single_line)
            return [line.substring(pos_sp + 1).trim()]

        this.line_idx += 2
        let s_multiline = []
        while (this.line_idx < lines.length && lines[this.line_idx] !== '"""') {
            s_multiline.push(lines[this.line_idx])
            this.line_idx = this.line_idx + 1
        }
        return s_multiline
    }

    get_compiled_fif_filename() {
        return this.artifacts_folder + "/compiled.fif"
    }

    get_runner_fif_filename() {
        return this.artifacts_folder + "/runner.fif"
    }

    async run_and_check() {
        const wasmModule = await compileWasm(TOLKFIFTLIB_MODULE, TOLKFIFTLIB_WASM)
        let res = compileFile(wasmModule, this.tolk_filename, this.experimental_options, this.enable_tolk_lines_comments)
        let exit_code = res.status === 'ok' ? 0 : 1
        let stderr = res.message || res.stderr
        let stdout = ''

        if (exit_code === 0 && this.compilation_should_fail)
            throw new TolkCompilationSucceededError("compilation succeeded, but it should have failed")

        for (let should_include of this.stderr_includes)  // @stderr is used to check errors and warnings
            should_include.check(stderr)

        if (exit_code !== 0 && this.compilation_should_fail)
            return

        if (exit_code !== 0 && !this.compilation_should_fail)
            throw new TolkCompilationFailedError(`tolk exit_code = ${exit_code}`, stderr)

        fs.writeFileSync(this.get_compiled_fif_filename(), `"Asm.fif" include\n${res.fiftCode}`)
        {
            let runner = `"${this.get_compiled_fif_filename()}" include <s constant code\n`
            for (let t of this.input_output)
                runner += `${t.input} ${t.method_id} code 1 runvmx abort"exitcode is not 0" .s cr { drop } depth 1- times\n`
            if (this.expected_hash !== null)
                runner += `"${this.get_compiled_fif_filename()}" include hash .s\n`
            fs.writeFileSync(this.get_runner_fif_filename(), runner)
        }

        res = child_process.spawnSync(FIFT_EXECUTABLE, [this.get_runner_fif_filename()])
        exit_code = res.status
        stderr = (res.stderr || res.error).toString()
        stdout = (res.stdout || '').toString()
        if (exit_code)
            throw new FiftExecutionFailedError(`fift exit_code = ${exit_code}`, stderr)

        let stdout_lines = stdout.split("\n").map(x => x.trim()).filter(s => s.length > 0)
        let fif_code_hash = null
        if (this.expected_hash !== null) { // then the last stdout line is a hash
            fif_code_hash = stdout_lines[stdout_lines.length - 1]
            stdout_lines = stdout_lines.slice(0, stdout_lines.length - 1)
        }

        if (stdout_lines.length !== this.input_output.length)
            throw new CompareOutputError(`unexpected number of fift output: ${stdout_lines.length} lines, but ${this.input_output.length} testcases`, stdout)

        for (let i = 0; i < stdout_lines.length; ++i)
            this.input_output[i].check(stdout_lines, i)

        if (this.fif_codegen.length) {
            const fif_output = fs.readFileSync(this.get_compiled_fif_filename(), 'utf-8').split(/\r?\n/)
            for (let fif_codegen of this.fif_codegen)
                fif_codegen.check(fif_output)
        }

        if (this.expected_hash !== null)
            this.expected_hash.check(fif_code_hash)
    }
}

async function run_all_tests(/**string[]*/ tests) {
    for (let ti = 0; ti < tests.length; ++ti) {
        let tolk_filename = tests[ti]
        print(`Running test ${ti + 1}/${tests.length}: ${path.basename(tolk_filename)}`)

        let artifacts_folder = path.join(TMP_DIR, tolk_filename)
        let testcase = new TolkTestFile(tolk_filename, artifacts_folder)

        try {
            if (!fs.existsSync(artifacts_folder))
                fs.mkdirSync(artifacts_folder, {recursive: true})
            testcase.parse_input_from_tolk_file()
            await testcase.run_and_check()
            fs.rmSync(artifacts_folder, {recursive: true})

            if (testcase.compilation_should_fail)
                print("  OK, stderr match")
            else
                print(`  OK, ${testcase.input_output.length} cases`)
        } catch (e) {
            if (e instanceof ParseInputError) {
                print(`  Error parsing input (cur line #${testcase.line_idx + 1}):`, e.message)
                process.exit(2)
            } else if (e instanceof TolkCompilationFailedError) {
                print("  Error compiling tolk:", e.message)
                print("  stderr:")
                print(e.stderr.trimEnd())
                process.exit(2)
            } else if (e instanceof FiftExecutionFailedError) {
                print("  Error executing fift:", e.message)
                print("  stderr:")
                print(e.stderr.trimEnd())
                print("  compiled.fif at:", testcase.get_compiled_fif_filename())
                process.exit(2)
            } else if (e instanceof CompareOutputError) {
                print("  Mismatch in output:", e.message)
                print("  Full output:")
                print(e.output.trimEnd())
                print("  Was compiled to:", testcase.get_compiled_fif_filename())
                process.exit(2)
            } else if (e instanceof CompareFifCodegenError) {
                print("  Mismatch in fif codegen:", e.message)
                print("  Was compiled to:", testcase.get_compiled_fif_filename())
                print(fs.readFileSync(testcase.get_compiled_fif_filename(), 'utf-8'))
                process.exit(2)
            } else if (e instanceof CompareCodeHashError) {
                print("  Mismatch in code hash:", e.message)
                print("  Was compiled to:", testcase.get_compiled_fif_filename())
                process.exit(2)
            }
            throw e
        }
    }
}

const tests = new CmdLineOptions(process.argv).find_tests()
print(`Found ${tests.length} tests`)
run_all_tests(tests).then(
    () => print(`Done, ${tests.length} tests`),
    console.error
)

// below are WASM helpers, which don't exist in Python version

process.setMaxListeners(0);

function copyToCString(mod, str) {
    const len = mod.lengthBytesUTF8(str) + 1;
    const ptr = mod._malloc(len);
    mod.stringToUTF8(str, ptr, len);
    return ptr;
}

function copyToCStringPtr(mod, str, ptr) {
    const allocated = copyToCString(mod, str);
    mod.setValue(ptr, allocated, '*');
    return allocated;
}

/** @return {string} */
function copyFromCString(mod, ptr) {
    return mod.UTF8ToString(ptr);
}

/** @return {{status: string, message: string, fiftCode: string, codeBoc: string, codeHashHex: string}} */
function compileFile(mod, filename, experimentalOptions, withSrcLineComments) {
    // see tolk-wasm.cpp: typedef void (*WasmFsReadCallback)(int, char const*, char**, char**)
    const callbackPtr = mod.addFunction((kind, dataPtr, destContents, destError) => {
        if (kind === 0) { // realpath
            try {
                let relative = copyFromCString(mod, dataPtr)
                if (relative.startsWith('@stdlib/')) {
                    // import "@stdlib/filename" or import "@stdlib/filename.tolk"
                    relative = STDLIB_FOLDER + '/' + relative.substring(7)
                    if (!relative.endsWith('.tolk')) {
                        relative += '.tolk'
                    }
                }
                copyToCStringPtr(mod, fs.realpathSync(relative), destContents);
            } catch (err) {
                copyToCStringPtr(mod, 'cannot find file', destError);
            }
        } else if (kind === 1) { // read file
            try {
                const absolute = copyFromCString(mod, dataPtr) // already normalized (as returned above)
                copyToCStringPtr(mod, fs.readFileSync(absolute).toString('utf-8'), destContents);
            } catch (err) {
                copyToCStringPtr(mod, err.message || err.toString(), destError);
            }
        } else {
            copyToCStringPtr(mod, 'Unknown callback kind=' + kind, destError);
        }
    }, 'viiii');

    const config = {
        optimizationLevel: 2,
        withStackComments: true,
        withSrcLineComments: withSrcLineComments,
        experimentalOptions: experimentalOptions || undefined,
        entrypointFileName: filename
    };

    const configPtr = copyToCString(mod, JSON.stringify(config));

    const responsePtr = mod._tolk_compile(configPtr, callbackPtr);

    return JSON.parse(copyFromCString(mod, responsePtr));
}

async function compileWasm(tolkFiftLibJsFileName, tolkFiftLibWasmFileName) {
    const wasmModule = require(tolkFiftLibJsFileName)
    const wasmBinary = new Uint8Array(fs.readFileSync(tolkFiftLibWasmFileName))

    return await wasmModule({ wasmBinary })
}
