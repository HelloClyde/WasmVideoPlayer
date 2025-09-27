const fs = require('fs');
const path = require('path');

describe('decoder wasm build', () => {
  const wasmPath = path.resolve(__dirname, '../libffmpeg.wasm');
  const gluePath = path.resolve(__dirname, '../libffmpeg.js');

  let wasmModule;
  let wasmInstance;

  const createImportObject = (module) => {
    const importObject = {};

    WebAssembly.Module.imports(module).forEach((imp) => {
      if (!importObject[imp.module]) {
        importObject[imp.module] = {};
      }

      switch (imp.kind) {
        case 'function':
          importObject[imp.module][imp.name] = jest.fn(() => 0);
          break;
        case 'global':
          importObject[imp.module][imp.name] = 0;
          break;
        case 'memory':
          importObject[imp.module][imp.name] = new WebAssembly.Memory({ initial: 1024, maximum: 2048 });
          break;
        case 'table':
          importObject[imp.module][imp.name] = new WebAssembly.Table({ element: 'anyfunc', initial: 64, maximum: 64 });
          break;
        default:
          throw new Error(`Unexpected import kind: ${imp.kind}`);
      }
    });

    return importObject;
  };

  beforeAll(() => {
    const wasmBytes = fs.readFileSync(wasmPath);
    wasmModule = new WebAssembly.Module(wasmBytes);
    wasmInstance = new WebAssembly.Instance(wasmModule, createImportObject(wasmModule));
  });

  test('emits wasm artifact with meaningful size', () => {
    const stats = fs.statSync(wasmPath);
    expect(stats.isFile()).toBe(true);
    expect(stats.size).toBeGreaterThan(1024 * 100); // > 100KiB ensures non-trivial build
  });

  test('wasm binary exposes imports and exports for the decoder runtime', () => {
    const exports = WebAssembly.Module.exports(wasmModule);
    const imports = WebAssembly.Module.imports(wasmModule);

    expect(exports.length).toBeGreaterThanOrEqual(10);
    expect(imports.length).toBeGreaterThan(0);

    const memoryExport = exports.find((exp) => exp.kind === 'memory');
    expect(memoryExport).toBeDefined();
  });

  test('wasm can be instantiated with stubbed dependencies', () => {
    expect(wasmInstance).toBeDefined();
    const exportEntries = Object.entries(wasmInstance.exports);

    expect(exportEntries.length).toBeGreaterThanOrEqual(10);
    const hasFunction = exportEntries.some(([, value]) => typeof value === 'function');
    expect(hasFunction).toBe(true);
  });

  test('exposes a 64MiB linear memory matching the build configuration', () => {
    const exportedMemory = Object.values(wasmInstance.exports).find((value) => value instanceof WebAssembly.Memory);
    expect(exportedMemory).toBeDefined();
    expect(exportedMemory.buffer.byteLength).toBeGreaterThanOrEqual(64 * 1024 * 1024);
  });

  test('glue code re-exports the decoder entry points', () => {
    const glue = fs.readFileSync(gluePath, 'utf8');
    const expectedSymbols = [
      '_initDecoder',
      '_uninitDecoder',
      '_openDecoder',
      '_closeDecoder',
      '_sendData',
      '_decodeOnePacket',
      '_seekTo',
      '_main',
      '_malloc',
      '_free',
    ];

    expectedSymbols.forEach((symbol) => {
      expect(glue.includes(symbol)).toBe(true);
    });
  });
});
