export function loadModule(bytes, textAbi = 'utf8', startup = null) {
    if (!['utf8', 'utf16-object'].includes(textAbi)) throw new Error(`unsupported text ABI: ${textAbi}`);
    const module = new WebAssembly.Module(bytes);
    const imports = WebAssembly.Module.imports(module);
    for (const entry of imports) {
        if (entry.module !== 'env' || entry.kind !== 'function' || !['set_text', 'set_number'].includes(entry.name))
            throw new Error(`unexpected import: ${entry.module}.${entry.name} (${entry.kind})`);
    }
    const decoder = new TextDecoder(textAbi === 'utf8' ? 'utf-8' : 'utf-16le', { fatal: true, ignoreBOM: true });
    const state = { texts: new Map(), numbers: new Map(), events: [], record: true };
    let exports;
    const instance = new WebAssembly.Instance(module, { env: {
        set_text(...args) {
            if (args.length !== 3) throw new Error('set_text requires exactly three Wasm arguments');
            const [id, pointer, length] = args;
            if (!Number.isInteger(length) || length < 0 || !exports?.memory)
                throw new Error('invalid text argument or unavailable memory');
            const address = pointer >>> 0;
            const buffer = exports.memory.buffer;
            const offset = textAbi === 'utf8' ? 0 : 8;
            const bytes = length * (textAbi === 'utf8' ? 1 : 2);
            if (address + offset + bytes > buffer.byteLength) throw new Error('text argument is out of bounds');
            if (offset) {
                const view = new DataView(buffer);
                if (view.getUint32(address, true) !== length || view.getUint32(address + 4, true) !== 5)
                    throw new Error('invalid UTF-16 object header');
            }
            const text = decoder.decode(new Uint8Array(buffer, address + offset, bytes));
            state.texts.set(id, text);
            if (state.record) state.events.push(['text', id, text]);
        },
        set_number(...args) {
            if (args.length !== 2) throw new Error('set_number requires exactly two Wasm arguments');
            const [id, value] = args;
            state.numbers.set(id, value);
            if (state.record) state.events.push(['number', id, value]);
        },
    } });
    exports = instance.exports;
    if (startup !== null) {
        if (typeof exports[startup] !== 'function' || exports[startup].length !== 0)
            throw new Error(`missing zero-argument startup function: ${startup}`);
        exports[startup]();
    }
    return { exports, state, module, imports, initialMemoryBytes: exports.memory?.buffer.byteLength ?? 0 };
}
