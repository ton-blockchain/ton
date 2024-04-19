// Usage: `node legacy_tests.js` from current dir, providing some env (see getenv() calls).
// This is a JS version of legacy_tester.py to test FunC compiled to WASM.

const fs = require('fs');
const path = require('path')
const process = require('process');
const { compileWasm, compileFile } = require('./wasm_tests_common');


/** @return {string} */
function getenv(name, def = null) {
    if (name in process.env)
        return process.env[name]
    if (def === null) {
        console.log(`Environment variable ${name} is not set`)
        process.exit(1)
    }
    return def
}

const FUNCFIFTLIB_MODULE = getenv('FUNCFIFTLIB_MODULE')
const FUNCFIFTLIB_WASM = getenv('FUNCFIFTLIB_WASM')
const TESTS_DIR = "legacy_tests"

/**
 * @return {{filename: string, code_hash: BigInt}[]}
 */
function load_legacy_tests_list(jsonl_filename) {
    let contents = fs.readFileSync(jsonl_filename)
    let results = [...contents.toString().matchAll(/^\[\s*"(.*?)"\s*,\s*(.*?)\s*]/gms)]
    return results.map((line) => ({
        filename: line[1].trim(),
        code_hash: BigInt(line[2]),
    }))
}

async function main() {
    const tests = load_legacy_tests_list('legacy_tests.jsonl')

    for (let ti = 0; ti < tests.length; ++ti) {
        const {filename: filename_rel, code_hash} = tests[ti]
        const filename = path.join(TESTS_DIR, filename_rel)
        console.log(`Running test ${ti + 1}/${tests.length}: ${filename_rel}`)

        if (filename.includes('storage-provider')) {
            console.log("  Skip");
            continue;
        }

        const wasmModule = await compileWasm(FUNCFIFTLIB_MODULE, FUNCFIFTLIB_WASM)
        const response = compileFile(wasmModule, filename);

        if (response.status !== 'ok') {
            console.error(response);
            throw new Error(`Could not compile ${filename}`);
        }

        if (BigInt('0x' + response.codeHashHex) !== code_hash) {
            throw new Error(`Code hash is different for ${filename}`);
        }

        console.log('  OK  ');
    }

    console.log(`Done ${tests.length}`)
}

main().catch(console.error)
