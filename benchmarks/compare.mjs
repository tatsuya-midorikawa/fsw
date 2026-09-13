import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { accessSync, constants, copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync } from 'node:fs';
import { tmpdir, platform, arch, release, cpus } from 'node:os';
import { basename, delimiter, dirname, extname, isAbsolute, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { brotliCompressSync, constants as zlib, gzipSync } from 'node:zlib';
import { loadModule } from './host.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const workloads = ['add', 'kernels', 'micro-app'];
const hash = value => createHash('sha256').update(value).digest('hex');
const now = () => process.hrtime.bigint();
const milliseconds = start => Number(now() - start) / 1e6;
const round = value => Number(value.toFixed(6));
export const quantile = (values, q) => [...values].sort((a, b) => a - b)[Math.floor((values.length - 1) * q)];

function identifier(value) {
    return typeof value === 'string' && /^[a-zA-Z][a-zA-Z0-9_-]*$/.test(value);
}

export function validateConfig(config) {
    if (!config || config.schema !== 1 || !Array.isArray(config.tools) || !config.tools.length)
        throw new Error('configuration requires schema: 1 and a non-empty tools array');
    for (const [name, fallback, minimum, maximum] of [
        ['samples', 15, 3, 1000], ['warmups', 3, 1, 100],
        ['runtime_samples', 7, 3, 100], ['timeout_ms', 180000, 1000, 600000],
    ]) {
        config[name] ??= fallback;
        if (!Number.isInteger(config[name]) || config[name] < minimum || config[name] > maximum)
            throw new Error(`${name} must be an integer from ${minimum} to ${maximum}`);
    }
    const ids = new Set();
    for (const tool of config.tools) {
        if (!tool || !identifier(tool.id) || ids.has(tool.id)) throw new Error('tool ids must be unique identifiers');
        ids.add(tool.id);
        if (typeof tool.label !== 'string' || !tool.label || !/^[a-z0-9]+$/.test(tool.extension ?? ''))
            throw new Error(`invalid label or source extension for ${tool.id}`);
        const commands = [tool.version, ...Object.values(tool.build ?? {})];
        if (commands.length < 2 || commands.some(c => !Array.isArray(c) || !c.length || c.some(a => typeof a !== 'string')))
            throw new Error(`${tool.id} requires version and build command arrays`);
        for (const profile of Object.keys(tool.build)) if (!identifier(profile)) throw new Error('invalid profile name');
        if (!['utf8', 'utf16-object'].includes(tool.text_abi ?? 'utf8')) throw new Error('invalid text_abi');
        if (tool.startup !== undefined && tool.startup !== null && typeof tool.startup !== 'string')
            throw new Error('startup must be a function name or null');
        if (tool.env && (typeof tool.env !== 'object' || Array.isArray(tool.env) || Object.values(tool.env).some(v => typeof v !== 'string')))
            throw new Error('environment values must be strings');
        if (tool.path && (!Array.isArray(tool.path) || tool.path.some(v => typeof v !== 'string')))
            throw new Error('path must be an array of directories');
        if (tool.auxiliaries !== undefined && !Array.isArray(tool.auxiliaries))
            throw new Error('auxiliaries must be an array of filenames');
        if (tool.components !== undefined && !Array.isArray(tool.components))
            throw new Error('components must be an array');
        for (const auxiliary of tool.auxiliaries ?? [])
            if (typeof auxiliary !== 'string' || auxiliary !== basename(auxiliary) || auxiliary === '.' || auxiliary === '..')
                throw new Error('auxiliaries must be plain filenames');
        for (const component of tool.components ?? [])
            if (!component.name || !Array.isArray(component.version) || !component.version.length ||
                component.version.some(v => typeof v !== 'string'))
                throw new Error('components require name and version command');
    }
    return config;
}

function expand(text, values) {
    return text.replace(/\{([a-z_]+)\}/g, (_, key) => {
        if (!(key in values)) throw new Error(`unknown command placeholder: {${key}}`);
        return values[key];
    });
}

function environment(tool, values) {
    const env = { ...process.env };
    for (const [key, value] of Object.entries(tool.env ?? {})) env[key] = expand(value, values);
    if (tool.path?.length) {
        const pathKey = Object.keys(env).find(key => key.toLowerCase() === 'path') ?? 'PATH';
        env[pathKey] = [...tool.path.map(p => expand(p, values)), env[pathKey] ?? ''].join(delimiter);
    }
    return env;
}

function execute(command, values, env, timeout) {
    const args = command.map(value => expand(value, values));
    const result = spawnSync(args[0], args.slice(1), {
        cwd: values.directory, env, encoding: 'utf8', timeout, maxBuffer: 8 * 1024 * 1024, shell: false,
    });
    if (result.error) throw new Error(`cannot execute ${basename(args[0])}: ${result.error.message}`);
    if (result.status !== 0)
        throw new Error(`${basename(args[0])} exited ${result.status ?? result.signal}:\n${result.stderr}\n${result.stdout}`);
    return result;
}

function executable(command, env) {
    const pathValue = Object.entries(env).find(([key]) => key.toLowerCase() === 'path')?.[1] ?? '';
    const paths = isAbsolute(command) || command.includes('/') || command.includes('\\') ?
        [resolve(command)] : pathValue.split(delimiter).map(p => join(p, command));
    const suffixes = process.platform === 'win32' && !extname(command) ?
        ['', ...(env.PATHEXT ?? '.EXE;.CMD;.BAT').split(';')] : [''];
    for (const path of paths) for (const suffix of suffixes) {
        const candidate = path + suffix;
        if (!existsSync(candidate) || !statSync(candidate).isFile()) continue;
        accessSync(candidate, process.platform === 'win32' ? constants.F_OK : constants.X_OK);
        return candidate;
    }
    throw new Error(`cannot resolve compiler executable: ${command}`);
}

function fibonacci(n) {
    if (n < 2) return n;
    let a = 0, b = 1;
    for (let i = 0; i < n; ++i) [a, b] = [b, (a + b) | 0];
    return a;
}

function checksum(n, seed) {
    let value = seed;
    for (let i = 0; i < n; ++i) value = ((value ^ (value << 5)) + (i ^ (value >> 2))) | 0;
    return value;
}

export function verify(bytes, workload, tool) {
    assert.ok(WebAssembly.validate(bytes), `${tool.label}: invalid WebAssembly`);
    const loaded = loadModule(bytes, tool.text_abi ?? 'utf8', tool.startup ?? null);
    const { exports: e, state } = loaded;
    function signature(name, arity) {
        assert.equal(typeof e[name], 'function', `${tool.label}: missing ${name}`);
        assert.equal(e[name].length, arity, `${tool.label}: wrong ${name} arity`);
    }
    if (workload === 'add' || workload === 'kernels') {
        signature('Add', 2);
        for (const [a, b] of [[20, 22], [2147483647, 1], [-2147483648, -1], [1.75, 2.5]])
            assert.equal(e.Add(a, b), ((a | 0) + (b | 0)) | 0, `${tool.label}: Add/i32 ABI mismatch`);
    }
    if (workload === 'kernels') {
        signature('Fibonacci', 1);
        signature('Checksum', 2);
        for (const n of [-2, 0, 1, 2, 2.75, 10, 20, 28])
            assert.equal(e.Fibonacci(n), fibonacci(n | 0), `${tool.label}: Fibonacci mismatch`);
        for (const n of [-1, 0, 1, 31, 1000])
            for (const seed of [0, 42, -1, 2147483647, -2147483648])
                assert.equal(e.Checksum(n, seed), checksum(n, seed), `${tool.label}: Checksum mismatch`);
    }
    if (workload === 'micro-app') {
        signature('InitApp', 0);
        signature('RunComputation', 1);
        signature('Render', 1);
        assert.equal(e.InitApp(), undefined);
        assert.deepEqual(state.events, [['text', 0, 'Ready (WebAssembly)'], ['number', 1, 0]]);
        for (const n of [0, 1, 2, 10, 20]) {
            state.events.length = 0;
            assert.equal(e.RunComputation(n), fibonacci(n));
            assert.equal(e.Render(n), fibonacci(n));
            assert.deepEqual(state.events, [
                ['text', 0, `${n % 2 === 0 ? 'Even' : 'Odd'}: café / 日本語 / 😀`],
                ['number', 1, fibonacci(n)],
            ], `${tool.label}: DOM/Unicode behavior mismatch`);
        }
    }
    return loaded;
}

function compressed(bytes) {
    return {
        bytes: bytes.length,
        gzip_bytes: gzipSync(bytes, { level: 9 }).length,
        brotli_bytes: brotliCompressSync(bytes, { params: { [zlib.BROTLI_PARAM_QUALITY]: 11 } }).length,
        sha256: hash(bytes),
    };
}

function runtime(loaded, workload, samples) {
    const { exports: e, state } = loaded;
    state.record = false;
    const cases = workload === 'add' ? [['Add', [20, 22], 100000, 42]] :
        workload === 'kernels' ? [['Fibonacci', [28], 5, fibonacci(28)], ['Checksum', [10000, 42], 100, checksum(10000, 42)]] :
        [['RunComputation', [28], 5, fibonacci(28)], ['Render', [20], 100, fibonacci(20)]];
    return cases.map(([name, args, iterations, expected]) => {
        const fn = e[name];
        for (let i = 0; i < 10; ++i) assert.equal(fn(...args), expected);
        const times = [];
        for (let sample = 0; sample < samples; ++sample) {
            const start = now();
            let value;
            for (let i = 0; i < iterations; ++i) value = fn(...args);
            times.push(milliseconds(start) * 1e6 / iterations);
            assert.equal(value, expected);
        }
        return { export: name, args, iterations, median_ns: round(quantile(times, 0.5)), p90_ns: round(quantile(times, 0.9)), samples_ns: times.map(round) };
    });
}

export function compare(config) {
    validateConfig(config);
    const fixtures = config.fixtures ? resolve(config.fixtures) : join(root, 'benchmarks/fixtures');
    const selected = config.workloads ?? workloads;
    if (!Array.isArray(selected) || !selected.length || new Set(selected).size !== selected.length ||
        selected.some(name => !workloads.includes(name))) throw new Error('invalid workload selection');
    const directory = mkdtempSync(join(tmpdir(), 'fsw-toolchains-'));
    const cells = [], versions = [];
    try {
        for (const tool of config.tools) {
            const cache = join(directory, 'cache', tool.id);
            mkdirSync(cache, { recursive: true });
            const values = { root, directory, cache };
            const env = environment(tool, values);
            const version = execute(tool.version, values, env, config.timeout_ms);
            const components = (tool.components ?? []).map(component => {
                const result = execute(component.version, values, env, config.timeout_ms);
                return { name: component.name, version: (result.stdout + result.stderr).trim() };
            });
            const binary = executable(expand(Object.values(tool.build)[0][0], values), env);
            versions.push({
                id: tool.id, label: tool.label, version: (version.stdout + version.stderr).trim(),
                executable: basename(binary), executable_bytes: statSync(binary).size, executable_sha256: hash(readFileSync(binary)),
                components, text_abi: tool.text_abi ?? 'utf8', startup: tool.startup ?? null,
                profiles: Object.fromEntries(Object.entries(tool.build).map(([name, command]) => [name, [basename(command[0]), ...command.slice(1)]])),
            });
            for (const [profile, command] of Object.entries(tool.build)) for (const workload of selected) {
                const work = join(directory, tool.id, profile, workload);
                mkdirSync(work, { recursive: true });
                const source = join(work, `input.${tool.extension}`), output = join(work, 'module.wasm');
                const fixture = join(fixtures, `${workload}.${tool.extension}`);
                copyFileSync(fixture, source);
                const local = { root, directory: work, cache, source, output };
                const cell = { tool, profile, workload, command, values: local, env: environment(tool, local), wall: [], first_ms: 0 };
                const start = now();
                execute(command, local, cell.env, config.timeout_ms);
                cell.first_ms = milliseconds(start);
                cell.bytes = readFileSync(output);
                verify(cell.bytes, workload, tool);
                for (let i = 0; i < config.warmups; ++i) {
                    execute(command, local, cell.env, config.timeout_ms);
                    assert.deepEqual(readFileSync(output), cell.bytes,
                        `${tool.label}/${profile}/${workload}: non-deterministic warmup output`);
                }
                cells.push(cell);
            }
        }
        // Rotate the order so one tool is not consistently measured first or last.
        for (let sample = 0; sample < config.samples; ++sample) {
            for (let position = 0; position < cells.length; ++position) {
                const cell = cells[(position + sample) % cells.length];
                const start = now();
                execute(cell.command, cell.values, cell.env, config.timeout_ms);
                cell.wall.push(milliseconds(start));
                assert.deepEqual(readFileSync(cell.values.output), cell.bytes,
                    `${cell.tool.label}/${cell.profile}/${cell.workload}: non-deterministic output`);
            }
        }
        const results = cells.map(cell => {
            const loaded = verify(cell.bytes, cell.workload, cell.tool);
            const auxiliaries = (cell.tool.auxiliaries ?? []).map(name => ({
                file: name, ...compressed(readFileSync(join(cell.values.directory, name))),
                required_by_this_workload: false,
            }));
            return {
                tool: cell.tool.id, label: cell.tool.label, profile: cell.profile, workload: cell.workload,
                source: { bytes: statSync(cell.values.source).size, sha256: hash(readFileSync(cell.values.source)) },
                wasm: compressed(cell.bytes), generated_auxiliaries: auxiliaries,
                required_compiler_auxiliary_bytes: 0,
                initial_memory_bytes: loaded.initialMemoryBytes,
                imports: loaded.imports, exports: WebAssembly.Module.exports(loaded.module),
                first_observed_build_ms: round(cell.first_ms),
                build_median_ms: round(quantile(cell.wall, 0.5)), build_p90_ms: round(quantile(cell.wall, 0.9)),
                build_samples_ms: cell.wall.map(round),
                runtime: runtime(loaded, cell.workload, config.runtime_samples),
            };
        });
        return {
            schema: 1, timestamp: new Date().toISOString(),
            runner_sha256: hash(readFileSync(fileURLToPath(import.meta.url))),
            configuration_sha256: hash(JSON.stringify(config)),
            host: { os: platform(), release: release(), arch: arch(), cpu: cpus()[0]?.model, node: process.version },
            protocol: {
                samples: config.samples, warmups: config.warmups, runtime_samples: config.runtime_samples,
                build: 'fresh process; warm, reused compiler caches; rotated run order; output checked after every build',
                first_build: 'first observed build of each isolated case; not a cold-machine or toolchain-install measurement',
                micro_app: 'identical Unicode text/number events; UTF-8 data pointers or UTF-16 object pointers; no dynamic slicing or browser painting',
                auxiliary: 'generated files reported separately; all modules validated without compiler-specific JavaScript; shared host adapter required for micro-app',
            },
            shared_host_adapter: compressed(readFileSync(join(root, 'benchmarks/host.mjs'))),
            tools: versions, results,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    try {
        if (process.argv.length !== 3) throw new Error('usage: node benchmarks/compare.mjs configuration.json');
        const config = JSON.parse(readFileSync(resolve(process.argv[2]), 'utf8'));
        console.log(JSON.stringify(compare(config), null, 2));
    } catch (error) {
        console.error(`benchmark failed: ${error.message}`);
        process.exitCode = 1;
    }
}
