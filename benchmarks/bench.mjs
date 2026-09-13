import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, statSync, rmSync } from 'node:fs';
import { tmpdir, cpus, platform, arch, release } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

const compiler = resolve(process.argv[2] ?? (process.platform === 'win32' ? 'build/Release/fsw.exe' : 'build/fsw'));
const samples = Number(process.argv[3] ?? 21);
if (!Number.isInteger(samples) || samples < 3 || samples > 1000) throw new Error('samples must be an integer from 3 to 1000');
const directory = mkdtempSync(join(tmpdir(), 'fsw-bench-'));
const now = () => process.hrtime.bigint();
const elapsed = start => Number(now() - start) / 1e6;
const quantile = (values, q) => [...values].sort((a, b) => a - b)[Math.floor((values.length - 1) * q)];
const round = value => Number(value.toFixed(4));
const hash = value => createHash('sha256').update(value).digest('hex');

function command(args) {
    const result = spawnSync(compiler, args, { encoding: 'utf8', timeout: 60_000 });
    assert.ifError(result.error);
    assert.equal(result.status, 0, result.stderr);
    return result;
}

function measure(source, verify) {
    const input = join(directory, 'workload.fs');
    const output = join(directory, 'workload.wasm');
    writeFileSync(input, source);
    for (let i = 0; i < 3; i++) command([input, '-o', output]);
    const wall = [], core = [];
    for (let i = 0; i < samples; i++) {
        const start = now();
        const result = command([input, '-o', output, '--stats']);
        wall.push(elapsed(start));
        core.push(JSON.parse(result.stderr).compile_ms);
    }
    const bytes = readFileSync(output);
    const { exports } = new WebAssembly.Instance(new WebAssembly.Module(bytes));
    const initialMemory = exports.memory?.buffer.byteLength ?? 0;
    verify(exports);
    return {
        source_bytes: Buffer.byteLength(source),
        source_sha256: hash(source),
        wasm_bytes: bytes.length,
        wasm_sha256: hash(bytes),
        initial_memory_bytes: initialMemory,
        wall_median_ms: round(quantile(wall, 0.5)),
        wall_p90_ms: round(quantile(wall, 0.9)),
        compiler_median_ms: round(quantile(core, 0.5)),
        compiler_p90_ms: round(quantile(core, 0.9)),
        exports,
    };
}

function runtime(fn, args, iterations, expected) {
    for (let i = 0; i < 10; i++) assert.equal(fn(...args), expected);
    const times = [];
    let value;
    for (let sample = 0; sample < 7; sample++) {
        const start = now();
        for (let i = 0; i < iterations; i++) value = fn(...args);
        times.push(elapsed(start) / iterations);
        assert.equal(value, expected);
    }
    return { iterations, median_ns_per_call: Number((quantile(times, 0.5) * 1e6).toFixed(2)) };
}

try {
    const add = measure('let Add a b = a + b\n', e => assert.equal(e.Add(20, 22), 42));
    const kernels = measure(readFileSync(fileURLToPath(new URL('../examples/math.fs', import.meta.url)), 'utf8'), e => {
        assert.equal(e.Fibonacci(20), 6765);
        assert.equal(e.FibonacciFast(40), 102334155);
        assert.equal(e.Sum(100), 5050);
    });
    const bulk = measure(Array.from({ length: 1000 }, (_, i) => `let f${i} x = x + ${i}\n`).join(''),
        e => assert.equal(e.f999(42), 1041));
    const arrays = measure(readFileSync(fileURLToPath(new URL('../examples/arrays.fs', import.meta.url)), 'utf8'), e => {
        const pointer = e.create(3);
        [1.25, 2.5, 3.75].forEach((value, i) => e.set(pointer, i, value));
        assert.equal(e.sum(pointer), 7.5);
    });
    const execution = {
        fibonacci_28: runtime(kernels.exports.Fibonacci, [28], 5, 317811),
        fibonacci_fast_40: runtime(kernels.exports.FibonacciFast, [40], 100_000, 102334155),
        sum_10000: runtime(kernels.exports.Sum, [10_000], 1000, 50005000),
    };
    for (const workload of [add, kernels, bulk, arrays]) delete workload.exports;
    console.log(JSON.stringify({
        schema: 1,
        timestamp: new Date().toISOString(),
        platform: { os: platform(), release: release(), arch: arch(), cpu: cpus()[0]?.model, node: process.version },
        compiler: {
            version: command(['--version']).stdout.trim(),
            executable_bytes: statSync(compiler).size,
            sha256: hash(readFileSync(compiler)),
        },
        samples,
        warmups: 3,
        workloads: { add, kernels, bulk_1000: bulk, arrays },
        execution,
    }, null, 2));
} finally {
    rmSync(directory, { recursive: true, force: true });
}
