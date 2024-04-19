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
 */
class FuncTestCaseStderrIncludes {
    constructor(/**string*/ expected_substr) {
        this.expected_substr = expected_substr
    }

    check(/**string*/ stderr) {
        if (!stderr.includes(this.expected_substr))
            throw new CompareOutputError(`pattern '${this.expected_substr}' not found in stderr`, stderr)
    }
}

class FuncTestFile {
    constructor(/**string*/ func_filename, /**string*/ artifacts_folder) {
        this.func_filename = func_filename
        this.artifacts_folder = artifacts_folder
        this.compilation_should_fail = false
        /** @type {FuncTestCaseStderrIncludes[]} */
        this.stderr_includes = []
        /** @type {FuncTestCaseInputOutput[]} */
        this.input_output = []
    }

    parse_input_from_func_file() {
        const lines = fs.readFileSync(this.func_filename, 'utf-8').split(/\r?\n/)
        let i = 0
        while (i < lines.length) {
            const line = lines[i]
            if (line.startsWith('TESTCASE')) {
                let s = line.split("|").map(p => p.trim())
                if (s.length !== 4)
                    throw new ParseInputError(`incorrect format of TESTCASE: ${line}`)
                this.input_output.push(new FuncTestCaseInputOutput(s[1], s[2], s[3]))
            } else if (line.startsWith('@compilation_should_fail')) {
                this.compilation_should_fail = true
            } else if (line.startsWith('@stderr')) {
                this.stderr_includes.push(new FuncTestCaseStderrIncludes(line.substring(7).trim()))
            }
            i++
        }

        if (this.input_output.length === 0 && !this.compilation_should_fail)
            throw new ParseInputError("no TESTCASE present")
        if (this.input_output.length !== 0 && this.compilation_should_fail)
            throw new ParseInputError("TESTCASE present, but compilation_should_fail")
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
                print("  Error parsing input:", e.message)
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
