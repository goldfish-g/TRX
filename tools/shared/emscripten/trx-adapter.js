// TRX WebGL adapter loader.
//
// Thin wrapper around the Emscripten-generated trx-engine.js. Exposes a
// promise-based API so any frontend can boot the engine without poking
// at Module internals.
//
// Usage:
//   import { createTRX } from './trx-adapter.js';
//   const engine = await createTRX({
//       canvas: document.getElementById('canvas'),
//       adapters: {
//           startGate: async () => { /* ... */ },
//           mountPersistence: async (path) => { /* ... */ },
//           syncPersistenceIn: async () => { /* ... */ },
//           syncPersistenceOut: (src, dst) => { /* ... */ },
//           flushPersistence: () => { /* ... */ },
//           loadModData: async (modName) => { /* ... */ },
//           selectProfile: async () => ({ mod, engine }),
//       },
//   });
//   await engine.run(['--engine', '1', '--mod', 'tr1']);
//
// The adapter object is contract-checked at load time. See
// trx-adapter.d.ts for the full type definition.

const REQUIRED_ADAPTERS = [
    'startGate',
    'mountPersistence',
    'syncPersistenceIn',
    'syncPersistenceOut',
    'flushPersistence',
    'loadModData',
    'selectProfile',
];

function validateAdapters(adapters) {
    if (adapters == null || typeof adapters !== 'object') {
        throw new Error('createTRX: adapters object is required');
    }
    for (const name of REQUIRED_ADAPTERS) {
        if (typeof adapters[name] !== 'function') {
            throw new Error(
                `createTRX: adapters.${name} must be a function`);
        }
    }
}

export async function createTRX(options) {
    if (options == null || typeof options !== 'object') {
        throw new Error('createTRX: options object is required');
    }
    const { canvas, adapters } = options;
    if (!(canvas instanceof HTMLCanvasElement)) {
        throw new Error('createTRX: options.canvas must be an HTMLCanvasElement');
    }
    validateAdapters(adapters);

    const wasmUrl = options.wasmUrl || 'TRX.wasm';
    const dataUrl = options.dataUrl || 'TRX.data';
    const jsUrl = options.jsUrl || 'trx-engine.js';

    // Deferred settlement of engine.run(): resolves when main() actually
    // returns (onExit) or rejects if the runtime aborts.
    let resolveRun = null;
    let rejectRun = null;
    const runSettled = new Promise((res, rej) => {
        resolveRun = res;
        rejectRun = rej;
    });

    // Module pre-config: installed before the Emscripten runtime reads it.
    const Module = {
        canvas,
        noInitialRun: true,
        // Let onExit fire when main() returns; with the default
        // noExitRuntime=true, main-return would be silently ignored and
        // engine.run() would never settle.
        noExitRuntime: false,
        trxAdapters: adapters,
        onExit: (status) => resolveRun?.(status | 0),
        onAbort: (reason) =>
            rejectRun?.(
                new Error(
                    typeof reason === 'string' ? reason : 'engine aborted',
                ),
            ),
        locateFile: (path) => {
            if (path.endsWith('.wasm')) return wasmUrl;
            if (path.endsWith('.data')) return dataUrl;
            return path;
        },
        print: options.onStdout || ((text) => console.log(text)),
        printErr: options.onStderr || ((text) => console.warn(text)),
    };

    // Load the Emscripten JS glue via <script>. We build with
    // MODULARIZE=1 + EXPORT_NAME=TRX, which exposes the factory as
    // globalThis.TRX.
    await loadScript(jsUrl);

    const factory = globalThis.TRX;
    if (typeof factory !== 'function') {
        throw new Error(
            `createTRX: ${jsUrl} did not expose a globalThis.TRX factory`);
    }

    const instance = await factory(Module);

    const engine = {
        canvas: instance.canvas,
        fs: instance.FS,
        version: readVersion(instance),
        _instance: instance,
        _running: null,

        run(args) {
            if (this._running !== null) {
                throw new Error('engine.run: already running');
            }
            const argv = Array.isArray(args) ? args : [];
            // callMain kicks off main(); with Asyncify, it returns
            // synchronously while main() continues yielding. The real
            // completion signal is Module.onExit (wired above).
            try {
                instance.callMain(argv);
            } catch (err) {
                if (err && err.name === 'ExitStatus') {
                    resolveRun?.(err.status || 0);
                } else {
                    rejectRun?.(err);
                }
            }
            this._running = runSettled;
            return runSettled;
        },

        syncViewport() {
            // Notify SDL/Emscripten that the canvas has been resized.
            // Consumer should ensure the canvas element has correct
            // CSS/px dimensions first.
            window.dispatchEvent(new Event('resize'));
        },
    };

    return engine;
}

function loadScript(url) {
    return new Promise((resolve, reject) => {
        const existing = document.querySelector(`script[data-trx-src="${url}"]`);
        if (existing) {
            resolve();
            return;
        }
        const el = document.createElement('script');
        el.src = url;
        el.async = true;
        el.dataset.trxSrc = url;
        el.onload = () => resolve();
        el.onerror = () => reject(new Error(`failed to load ${url}`));
        document.head.appendChild(el);
    });
}

function readVersion(instance) {
    try {
        const ptr = instance._trx_get_version();
        if (!ptr) return 'unknown';
        return instance.UTF8ToString(ptr);
    } catch (_) {
        return 'unknown';
    }
}
