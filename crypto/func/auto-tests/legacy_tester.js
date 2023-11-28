const fs = require('fs/promises');
const { compileWasm, compileFile } = require('./wasm_tests_common');

async function main() {
    const tests = JSON.parse((await fs.readFile('../legacy_tests.json')).toString('utf-8'))

    for (const [filename, hashstr] of tests) {
        if (filename.includes('storage-provider')) continue;

        const mod = await compileWasm()

        const response = await compileFile(mod, filename);

        if (response.status !== 'ok') {
            console.error(response);
            throw new Error('Could not compile ' + filename);
        }

        if (BigInt('0x' + response.codeHashHex) !== BigInt(hashstr)) {
            throw new Error('Compilation result is different for ' + filename);
        }

        console.log(filename, 'ok');
    }
}

main()