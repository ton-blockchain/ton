// Usage: `node run_tests.js tests_dir` OR `node run_tests.js test_file.fc`
// from current dir, providing some env (see getenv() calls).
// This is a JS version of run_tests.py to test FunC compiled to WASM.
// Don't forget to keep it identical to Python version!

const fs = require('fs');
const os = require('os');
const path = require('path');
const { compileWasm, compileFile } = require('./wasm_tests_common');
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

const FUNCFIFTLIB_MODULE = getenv('FUNCFIFTLIB_MODULE')
const FUNCFIFTLIB_WASM = getenv('FUNCFIFTLIB_WASM')
const FIFT_EXECUTABLE = getenv('FIFT_EXECUTABLE')
const FIFT_LIBS_FOLDER = getenv('FIFTPATH')  // this env is needed for fift to work properly
const TMP_DIR = os.tmpdir()

class CmdLineOptions {
    constructor(/**string[]*/ argv) {
        if (argv.length !== 3) {
            print("Usage: node run_tests.js tests_dir OR node run_tests.js test_file.fc")
            process.exit(1)
        }
        if (!fs.existsSync(argv[2])) {
            print(`Input '${argv[2]}' doesn't exist`)
            process.exit(1)
        }

        if (fs.lstatSync(argv[2]).isDirectory()) {
            this.tests_dir = argv[2]
            this.test_file = null
        } else {
            this.tests_dir = path.dirname(argv[2])
            this.test_file = argv[2]
        }
    }

    /** @return {string[]} */
    find_tests() {
        if (this.test_file)  // an option to run (debug) a single test
            return [this.test_file]

        let tests = fs.readdirSync(this.tests_dir).filter(f => f.endsWith('.fc') || f.endsWith(".func"))
        tests.sort()
        return tests.map(f => path.join(this.tests_dir, f))
    }
}


class ParseInputError extends Error {
}

class FuncCompilationFailedError extends Error {
    constructor(/**string*/ message, /**string*/ stderr) {
        super(message);
        this.stderr = stderr
    }
}

class FuncCompilationSucceededError extends Error {
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


/*
 * In positive tests, there are several testcases "input X should produce output Y".
 */
class FuncTestCaseInputOutput {
    static reJustNumber = /^[-+]?\d+$/
    static reMathExpr = /^[0x123456789()+\-*/<>]*$/

    constructor(/**string*/ method_id_str, /**string*/ input_str, /**string*/ output_str) {
        let processed_inputs = []
        for (let in_arg of input_str.split(' ')) {
            if (in_arg.length === 0)
                continue
            else if (in_arg.startsWith("x{") || FuncTestCaseInputOutput.reJustNumber.test(in_arg))
                processed_inputs.push(in_arg)
            else if (FuncTestCaseInputOutput.reMathExpr.test(in_arg))
                // replace "3<<254" with "3n<<254n" (big number) before eval (in Python we don't need this)
                processed_inputs.push(eval(in_arg.replace('//', '/').replace(/(\d)($|\D)/gmi, '$1n$2')).toString())
            else
                throw new ParseInputError(`'${in_arg}' can't be evaluated`)
        }

        this.method_id = +method_id_str
        this.input = processed_inputs.join(' ')
        this.expected_output = output_str
    }

    check(/**string[]*/ stdout_lines, /**number*/ line_idx) {
        if (stdout_lines[line_idx] !== this.expected_output)
            throw new CompareOutputError(`error on case ${line_idx + 1}: expected '${this.expected_output}', found '${stdout_lines[line_idx]}'`, stdout_lines.join("\n"))
    }
}

/*
 * @stderr checks, when compilation fails, that stderr (compilation error) is expected.
 * If it's multiline, all lines must be present in specified order.
 */
class FuncTestCaseStderr {
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
class FuncTestCaseFifCodegen {
    constructor(/**string[]*/ fif_pattern, /**boolean*/ avoid) {
        /** @type {string[]} */
        this.fif_pattern = fif_pattern.map(s => s.trim())
        this.avoid = avoid
    }

    check(/**string[]*/ fif_output) {
        // in case there are no comments at all (typically for wasm), drop them from fif_pattern
        const has_comments = fif_output.some(line => line.includes("//") && !line.includes("generated from"))
        if (!has_comments) {
            this.fif_pattern = this.fif_pattern.map(s => FuncTestCaseFifCodegen.split_line_to_cmd_and_comment(s)[0])
            this.fif_pattern = this.fif_pattern.filter(s => s !== '')
        }

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
            if (!FuncTestCaseFifCodegen.does_line_match(line_pattern, line_output))
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
        const [cmd_pattern, comment_pattern] = FuncTestCaseFifCodegen.split_line_to_cmd_and_comment(line_pattern)
        const [cmd_output, comment_output] = FuncTestCaseFifCodegen.split_line_to_cmd_and_comment(line_output.trim())
        return cmd_pattern === cmd_output && (comment_pattern === null || comment_pattern === comment_output)
    }
}


class FuncTestFile {
    constructor(/**string*/ func_filename, /**string*/ artifacts_folder) {
        this.line_idx = 0
        this.func_filename = func_filename
        this.artifacts_folder = artifacts_folder
        this.compilation_should_fail = false
        /** @type {FuncTestCaseStderr[]} */
        this.stderr_includes = []
        /** @type {FuncTestCaseInputOutput[]} */
        this.input_output = []
        /** @type {FuncTestCaseFifCodegen[]} */
        this.fif_codegen = []
    }

    parse_input_from_func_file() {
        const lines = fs.readFileSync(this.func_filename, 'utf-8').split(/\r?\n/)
        this.line_idx = 0

        while (this.line_idx < lines.length) {
            const line = lines[this.line_idx]
            if (line.startsWith('TESTCASE')) {
                let s = line.split("|").map(p => p.trim())
                if (s.length !== 4)
                    throw new ParseInputError(`incorrect format of TESTCASE: ${line}`)
                this.input_output.push(new FuncTestCaseInputOutput(s[1], s[2], s[3]))
            } else if (line.startsWith('@compilation_should_fail')) {
                this.compilation_should_fail = true
            } else if (line.startsWith('@stderr')) {
                this.stderr_includes.push(new FuncTestCaseStderr(this.parse_string_value(lines), false))
            } else if (line.startsWith("@fif_codegen_avoid")) {
                this.fif_codegen.push(new FuncTestCaseFifCodegen(this.parse_string_value(lines), true))
            } else if (line.startsWith("@fif_codegen")) {
                this.fif_codegen.push(new FuncTestCaseFifCodegen(this.parse_string_value(lines), false))
            }
            this.line_idx++
        }

        if (this.input_output.length === 0 && !this.compilation_should_fail)
            throw new ParseInputError("no TESTCASE present")
        if (this.input_output.length !== 0 && this.compilation_should_fail)
            throw new ParseInputError("TESTCASE present, but compilation_should_fail")
    }

    /** @return {string[]} */
    parse_string_value(/**string[]*/ lines) {
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
        const wasmModule = await compileWasm(FUNCFIFTLIB_MODULE, FUNCFIFTLIB_WASM)
        let res = compileFile(wasmModule, this.func_filename)
        let exit_code = res.status === 'ok' ? 0 : 1
        let stderr = res.message
        let stdout = ''

        if (exit_code === 0 && this.compilation_should_fail)
            throw new FuncCompilationSucceededError("compilation succeeded, but it should have failed")

        if (exit_code !== 0 && this.compilation_should_fail) {
            for (let should_include of this.stderr_includes)
                should_include.check(stderr)
            return
        }

        if (exit_code !== 0 && !this.compilation_should_fail)
            throw new FuncCompilationFailedError(`func exit_code = ${exit_code}`, stderr)

        fs.writeFileSync(this.get_compiled_fif_filename(), `"Asm.fif" include\n${res.fiftCode}`)
        {
            let runner = `"${this.get_compiled_fif_filename()}" include <s constant code\n`
            for (let t of this.input_output)
                runner += `${t.input} ${t.method_id} code 1 runvmx abort"exitcode is not 0" .s cr { drop } depth 1- times\n`
            fs.writeFileSync(this.get_runner_fif_filename(), runner)
        }

        res = child_process.spawnSync(FIFT_EXECUTABLE, [this.get_runner_fif_filename()])
        exit_code = res.status
        stderr = (res.stderr || res.error).toString()
        stdout = (res.stdout || '').toString()
        let stdout_lines = stdout.split("\n").map(x => x.trim()).filter(s => s.length > 0)

        if (exit_code)
            throw new FiftExecutionFailedError(`fift exit_code = ${exit_code}`, stderr)

        if (stdout_lines.length !== this.input_output.length)
            throw new CompareOutputError(`unexpected number of fift output: ${stdout_lines.length} lines, but ${this.input_output.length} testcases`, stdout)

        for (let i = 0; i < stdout_lines.length; ++i)
            this.input_output[i].check(stdout_lines, i)

        if (this.fif_codegen.length) {
            const fif_output = fs.readFileSync(this.get_compiled_fif_filename(), 'utf-8').split(/\r?\n/)
            for (let fif_codegen of this.fif_codegen)
                fif_codegen.check(fif_output)
        }
    }
}

async function run_all_tests(/**string[]*/ tests) {
    for (let ti = 0; ti < tests.length; ++ti) {
        let func_filename = tests[ti]
        print(`Running test ${ti + 1}/${tests.length}: ${func_filename}`)

        let artifacts_folder = path.join(TMP_DIR, func_filename)
        let testcase = new FuncTestFile(func_filename, artifacts_folder)

        try {
            if (!fs.existsSync(artifacts_folder))
                fs.mkdirSync(artifacts_folder, {recursive: true})
            testcase.parse_input_from_func_file()
            await testcase.run_and_check()
            fs.rmSync(artifacts_folder, {recursive: true})

            if (testcase.compilation_should_fail)
                print("  OK, compilation failed as it should")
            else
                print(`  OK, ${testcase.input_output.length} cases`)
        } catch (e) {
            if (e instanceof ParseInputError) {
                print(`  Error parsing input (cur line #${testcase.line_idx + 1}):`, e.message)
                process.exit(2)
            } else if (e instanceof FuncCompilationFailedError) {
                print("  Error compiling func:", e.message)
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
