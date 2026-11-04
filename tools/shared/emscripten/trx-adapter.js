// Loads the TRX browser engine for a page, which answers what the engine asks
// of it through adapters. See trx-adapter.d.ts for the contract.
//
//   import { createTRX } from './trx-adapter.js';
//
//   const engine = await createTRX({
//       canvas: document.getElementById('canvas'),
//       adapters: {
//           startGate: async () => { ... },
//           mountPersistence: async (path) => { ... },
//           syncPersistenceIn: async () => { ... },
//           flushPersistence: () => { ... },
//       },
//   });
//   await engine.run(['--mod', 'tr1']);

const REQUIRED_ADAPTERS = [
    'startGate',
    'mountPersistence',
    'syncPersistenceIn',
    'flushPersistence',
];

function validateAdapters(adapters) {
    if (adapters === null || typeof adapters !== 'object') {
        throw new Error('createTRX: options.adapters is required');
    }
    for (const name of REQUIRED_ADAPTERS) {
        if (typeof adapters[name] !== 'function') {
            throw new Error(`createTRX: adapters.${name} must be a function`);
        }
    }
}

function loadScript(url) {
    if (document.querySelector(`script[data-trx-src="${url}"]`) !== null) {
        return Promise.resolve();
    }
    return new Promise((resolve, reject) => {
        const script = document.createElement('script');
        script.src = url;
        script.async = true;
        script.dataset.trxSrc = url;
        script.onload = () => resolve();
        script.onerror = () => reject(new Error(`failed to load ${url}`));
        document.head.appendChild(script);
    });
}

export async function createTRX(options) {
    if (options === null || typeof options !== 'object') {
        throw new Error('createTRX: options are required');
    }
    const { canvas, adapters } = options;
    if (!(canvas instanceof HTMLCanvasElement)) {
        throw new Error('createTRX: options.canvas must be a canvas');
    }
    validateAdapters(adapters);

    const jsUrl = options.jsUrl ?? 'trx-engine.js';
    const wasmUrl = options.wasmUrl ?? 'TRX.wasm';
    const dataUrl = options.dataUrl ?? 'TRX.data';

    // Settles once main() returns, or when the engine aborts.
    let settleRun;
    const runDone = new Promise((resolve, reject) => {
        settleRun = { resolve, reject };
    });

    // The engine is built with MODULARIZE and EXPORT_NAME=TRX, so its code
    // exposes a factory as globalThis.TRX.
    await loadScript(jsUrl);
    if (typeof globalThis.TRX !== 'function') {
        throw new Error(`createTRX: ${jsUrl} exposes no TRX factory`);
    }

    const instance = await globalThis.TRX({
        canvas,
        trxAdapters: adapters,
        noInitialRun: true,
        // Lets main() returning end the runtime, which settles run().
        noExitRuntime: false,
        onExit: (status) => settleRun.resolve(status),
        onAbort: (reason) => settleRun.reject(new Error(String(reason))),
        locateFile: (path) => {
            if (path.endsWith('.wasm')) {
                return wasmUrl;
            }
            if (path.endsWith('.data')) {
                return dataUrl;
            }
            return path;
        },
        print: options.onStdout ?? ((text) => console.log(text)),
        printErr: options.onStderr ?? ((text) => console.warn(text)),
    });

    let isRunning = false;
    return {
        canvas,
        fs: instance.FS,
        idbfs: instance.IDBFS,
        version: instance.UTF8ToString(instance._trx_get_version()),

        run(args) {
            if (isRunning) {
                throw new Error('engine.run: the engine is already running');
            }
            isRunning = true;
            // With Asyncify, callMain returns as soon as main() first
            // sleeps; onExit reports when it actually returns.
            try {
                instance.callMain(Array.isArray(args) ? args : []);
            } catch (error) {
                if (error?.name === 'ExitStatus') {
                    settleRun.resolve(error.status);
                } else {
                    settleRun.reject(error);
                }
            }
            return runDone;
        },

        syncViewport() {
            // SDL follows the canvas on the window's resize event.
            window.dispatchEvent(new Event('resize'));
        },
    };
}
