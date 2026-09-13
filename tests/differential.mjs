import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const compiler = resolve(process.argv[2] ?? (process.platform === 'win32' ? 'build/Release/fsw.exe' : 'build/fsw'));
const directory = mkdtempSync(join(tmpdir(), 'fsw-reference-'));
const expressions = [
    ['2147483647 + 1', []],
    ['-2147483648', []],
    ['0xffffffff', []],
    ['-7 / 3', []],
    ['-7 % 3', []],
    ['(1 <<< -1) >>> 30', []],
    ['(1L <<< 63) >>> 62', []],
    ['1 + 2 <<< 3', []],
    ['8 ||| 3 &&& 1', []],
    ['1 ||| 2 ^^^ 3', []],
    ['8 <<< 1 &&& 3', []],
    ['1 + 2 |> abs = 3', []],
    ['not true && false', []],
    ['not false || true', []],
    ['let x: int = 1 in x = 1', []],
    ['let x = 20 in let y = x + 1 in y * 2', []],
    ['if true then 42 else 1 / 0', []],
    ['not false && true', []],
    ['int 42.9', []],
    ['int (0.0 / 0.0)', []],
    ['int (1.0 / 0.0)', []],
    ['int (-1.0 / 0.0)', []],
    ['int64 (1.0 / 0.0)', []],
    ['int64 (-1.0 / 0.0)', []],
    ['int 4294967295L', []],
    ['int64 -42', []],
    ['abs -42', []],
    ['min 42 43', []],
    ['max -42 -43', []],
    ['"日本語 😀".Length', []],
    ['("a" + "b").Length', []],
    ['"\\u0100" > "\\u00ff"', []],
    ['let a = [| 1; 2; 3 |] in a.[1]', []],
    ['let a = Array.zeroCreate<int> 3 in a.[0] <- 42; a.[0]', []],
    ['let a = Array.create 3 1.25 in int a.[2]', []],
    ['let a = Array.create 3 true in a.[2]', []],
    ['([||] : int array).Length', []],
    ['"\\uD800".Length', []],
    ['if true then 42\nelse 0', []],
    ['let mutable x = 0 in do (x <- 42); x', []],
    ['let mutable x = 0 in (do (x <- 42)); x', []],
    ['do 42', []],
    ['match 2 with | 1 | 2 -> 42 | _ -> 0', []],
    ['match "x" with | "x" -> 42 | _ -> 0', []],
    ['"😀" < "\\uE000"', []],
    ['-2147483648 % -1', []],
    ['-9223372036854775808L % -1L', []],
    ['-2147483648 / -1', []],
    ['-9223372036854775808L / -1L', []],
    ['1 / 0', []],
    ['1 % 0', []],
    ['abs -2147483648', []],
    ['abs -9223372036854775808L', []],
    ['let a = [|1|] in a.[-1]', []],
];

function execute(command, args) {
    const result = spawnSync(command, args, { encoding: 'utf8', timeout: 120_000 });
    assert.ifError(result.error);
    assert.equal(result.status, 0, `${command} failed:\n${result.stdout}\n${result.stderr}`);
    return result.stdout.trim();
}

try {
    const source = 'module Reference\n' +
        expressions.map(([expression], i) => `let case${i} () =\n    ${expression.replaceAll('\n', '\n    ')}`).join('\n') + '\n';
    writeFileSync(join(directory, 'case.fs'), source);
    writeFileSync(join(directory, 'reference.fsx'), '#load "case.fs"\nopen Reference\n' +
        expressions.map((_, i) => `try printfn "%A" (case${i} ()) with\n` +
            '| :? System.ArithmeticException -> printfn "trap:arithmetic"\n' +
            '| :? System.IndexOutOfRangeException -> printfn "trap:bounds"').join('\n') + '\n');
    const expected = execute('dotnet', ['fsi', '--exec', join(directory, 'reference.fsx')]).split(/\r?\n/);
    for (const flags of [[], ['--no-opt']]) {
        execute(compiler, [join(directory, 'case.fs'), '-o', join(directory, 'case.wasm'), ...flags]);
        const module = new WebAssembly.Module(readFileSync(join(directory, 'case.wasm')));
        const { exports } = new WebAssembly.Instance(module);
        expressions.forEach(([expression], i) => {
            let reference = expected[i];
            if (reference.startsWith('trap:')) {
                assert.throws(() => exports[`case${i}`](), WebAssembly.RuntimeError, `expected F# arithmetic/bounds failure: ${expression}`);
                return;
            }
            const actual = exports[`case${i}`]();
            if (reference === 'true') reference = '1';
            else if (reference === 'false') reference = '0';
            else if (reference === '()') reference = 'undefined';
            else reference = reference.replace(/L$/, '');
            assert.equal(String(actual), reference, `F# mismatch (${flags.join(' ') || 'optimized'}): ${expression}`);
        });
    }
    const examples = fileURLToPath(new URL('../examples/', import.meta.url)).replaceAll('\\', '/');
    writeFileSync(join(directory, 'examples.fsx'),
        `#load @"${examples}math.fs"\n#load @"${examples}arrays.fs"\n#load @"${examples}browser/app.fs"\n` +
        'printfn "%d" (Math.FibonacciFast 20)\nprintfn "%A" (Arrays.sum [| 1.25; 2.5; 3.75 |])\n');
    assert.deepEqual(execute('dotnet', ['fsi', '--exec', join(directory, 'examples.fsx')]).split(/\r?\n/), ['6765', '7.5']);
    console.log(`OK: ${expressions.length} expressions match the F# reference compiler in both optimization modes`);
} finally {
    rmSync(directory, { recursive: true, force: true });
}
