const fsSync = require('fs');

const copyToCString = (mod, str) => {
    const len = mod.lengthBytesUTF8(str) + 1;
    const ptr = mod._malloc(len);
    mod.stringToUTF8(str, ptr, len);
    return ptr;
};

const copyToCStringPtr = (mod, str, ptr) => {
    const allocated = copyToCString(mod, str);
    mod.setValue(ptr, allocated, '*');
    return allocated;
};

const copyFromCString = (mod, ptr) => {
    return mod.UTF8ToString(ptr);
};

async function compileFile(mod, filename) {
    const callbackPtr = mod.addFunction((_kind, _data, contents, error) => {
        const kind = copyFromCString(mod, _kind);
        const data = copyFromCString(mod, _data);
        if (kind === 'realpath') {
            copyToCStringPtr(mod, fsSync.realpathSync(data), contents);
        } else if (kind === 'source') {
            const path = fsSync.realpathSync(data);
            try {
                copyToCStringPtr(mod, fsSync.readFileSync(path).toString('utf-8'), contents);
            } catch (err) {
                copyToCStringPtr(mod, e.message, error);
            }
        } else {
            copyToCStringPtr(mod, 'Unknown callback kind ' + kind, error);
        }
    }, 'viiii');

    const config = {
        optLevel: 2,
        sources: [filename]
    };

    const configPtr = copyToCString(mod, JSON.stringify(config));

    const responsePtr = mod._func_compile(configPtr, callbackPtr);

    return JSON.parse(copyFromCString(mod, responsePtr));
}

const wasmModule = require(process.env.FUNCFIFTLIB_MODULE)

const wasmBinary = new Uint8Array(fsSync.readFileSync(process.env.FUNCFIFTLIB_WASM))

async function compileWasm() {
    const mod = await wasmModule({ wasmBinary })

    return mod
}

module.exports = {
    compileFile,
    compileWasm
}
