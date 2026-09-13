import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { compare, quantile, validateConfig } from '../benchmarks/compare.mjs';

const compiler = resolve(process.argv[2] ?? (process.platform === 'win32' ? 'build/Release/fsw.exe' : 'build/fsw'));
const tool = {
    id: 'fsw', label: 'fsw', extension: 'fs', text_abi: 'utf16-object',
    version: [compiler, '--version'],
    build: { test: [compiler, '{source}', '-o', '{output}'] },
};
const config = () => ({ schema: 1, samples: 3, warmups: 1, runtime_samples: 3, tools: [structuredClone(tool)] });
assert.equal(quantile([4, 1, 3, 2, 5], 0.5), 3);
assert.equal(quantile([4, 1, 3, 2, 5], 0.9), 4);
assert.throws(() => validateConfig({}), /schema/);
assert.throws(() => validateConfig({ ...config(), samples: 0 }), /samples/);
assert.throws(() => validateConfig({ ...config(), tools: [tool, tool] }), /unique/);
assert.throws(() => validateConfig({ ...config(), tools: [{ ...tool, text_abi: 'guess' }] }), /text_abi/);
assert.throws(() => validateConfig({ ...config(), tools: [{ ...tool, auxiliaries: ['../unsafe'] }] }), /filenames/);
assert.throws(() => validateConfig({ ...config(), tools: [{ ...tool, auxiliaries: 'runtime.js' }] }), /array/);
assert.throws(() => compare({ ...config(), workloads: ['unknown'] }), /workload/);

const report = compare(config());
assert.equal(report.schema, 1);
assert.match(report.runner_sha256, /^[0-9a-f]{64}$/);
assert.match(report.configuration_sha256, /^[0-9a-f]{64}$/);
assert.equal(report.results.length, 3);
assert.equal(report.results[0].wasm.bytes, 41);
assert.equal(report.results[0].initial_memory_bytes, 0);
assert.equal(report.results[0].required_compiler_auxiliary_bytes, 0);
assert.equal(report.results[2].initial_memory_bytes, 65536);
for (const result of report.results) {
    assert.equal(result.build_samples_ms.length, 3);
    assert.ok(result.build_median_ms > 0);
    assert.ok(result.wasm.gzip_bytes > 0 && result.wasm.brotli_bytes > 0);
    assert.match(result.wasm.sha256, /^[0-9a-f]{64}$/);
    for (const run of result.runtime) {
        assert.equal(run.samples_ns.length, 3);
        assert.ok(run.median_ns > 0);
    }
}

const fake = script => ({
    schema: 1, samples: 3, warmups: 1, runtime_samples: 3, workloads: ['add'],
    tools: [{
        id: 'fake', label: 'fake', extension: 'fs', version: [process.execPath, '--version'],
        build: { test: [process.execPath, '-e', script, '{output}'] },
    }],
});
assert.throws(() => compare(fake('process.exit(3)')), /exited 3/);
assert.throws(() => compare(fake("require('node:fs').writeFileSync(process.argv[1], 'invalid')")), /invalid WebAssembly/);
assert.throws(() => compare(fake("require('node:fs').writeFileSync(process.argv[1], Buffer.from([0,97,115,109,1,0,0,0]))")), /missing Add/);
assert.throws(() => compare({
    ...config(), workloads: ['micro-app'], tools: [{ ...tool, text_abi: 'utf8' }],
}), /deep-equal|Expected values|DOM/);
assert.throws(() => compare({
    ...config(), workloads: ['add'], tools: [{ ...tool, startup: 'missing' }],
}), /startup/);
assert.throws(() => compare({
    ...config(), workloads: ['add'], tools: [{ ...tool, auxiliaries: ['missing.js'] }],
}), /ENOENT/);
console.log('OK: comparison schema, 3 real workloads, compression, ABI checks and failure rejection');
