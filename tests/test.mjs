import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, rmSync, linkSync, readdirSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const compiler = resolve(process.argv[2] ?? (process.platform === 'win32' ? 'build/Release/fsw.exe' : 'build/fsw'));
const directory = mkdtempSync(join(tmpdir(), 'fsw-test-'));
let count = 0;

function build(source, flags = [], imports = {}) {
    const input = join(directory, 'test.fs');
    const output = join(directory, 'test.wasm');
    writeFileSync(input, source);
    const result = spawnSync(compiler, [input, '-o', output, ...flags], { encoding: 'utf8', timeout: 10_000 });
    assert.ifError(result.error);
    assert.equal(result.status, 0, `compilation failed:\n${result.stderr}\n${source}`);
    assert.equal(result.stderr, '');
    const bytes = readFileSync(output);
    assert.ok(WebAssembly.validate(bytes), `invalid WebAssembly for:\n${source}`);
    const module = new WebAssembly.Module(bytes);
    count++;
    return { exports: new WebAssembly.Instance(module, imports).exports, bytes, module };
}

function rejects(source, message, flags = []) {
    const input = join(directory, 'invalid.fs');
    const output = join(directory, 'preserve.wasm');
    writeFileSync(input, source);
    writeFileSync(output, 'preserve existing output');
    const result = spawnSync(compiler, [input, '-o', output, ...flags], { encoding: 'utf8', timeout: 10_000 });
    assert.ifError(result.error);
    assert.equal(result.status, 1, `expected diagnostic:\n${source}\n${result.stderr}`);
    assert.match(result.stderr, message);
    assert.equal(readFileSync(output, 'utf8'), 'preserve existing output');
    count++;
}

try {
    for (const flags of [[], ['--no-opt']]) {
        const { exports: arithmetic } = build(`
module Arithmetic
let add a b = a + b
let wide (a: int64) (b: int64) = a * b
let divide (a: float) (b: float) = a / b
let negative (a: float) = -a
let shift (a: int64) n = a >>> n
let wrapping () = 2147483647 + 1
let minimum () = -2147483648
let allBits () = 0xffffffff
let choose b x y = if b then x + 1 else y - 1
`, flags);
        assert.equal(arithmetic.add(20, 22), 42);
        assert.equal(arithmetic.add(2147483647, 1), -2147483648);
        assert.equal(arithmetic.wide(9007199254740993n, 3n), 27021597764222979n);
        assert.equal(arithmetic.divide(9, 2), 4.5);
        assert.ok(Object.is(arithmetic.negative(0), -0));
        assert.equal(arithmetic.shift(-8n, 1), -4n);
        assert.equal(arithmetic.shift(-8n, 65), -4n);
        assert.equal(arithmetic.wrapping(), -2147483648);
        assert.equal(arithmetic.minimum(), -2147483648);
        assert.equal(arithmetic.allBits(), -1);
        assert.equal(arithmetic.choose(1, 41, 0), 42);
        assert.equal(arithmetic.choose(0, 0, 43), 42);

        const { exports: loops } = build(`
let sum n =
    let mutable total = 0
    for i = 1 to n do
        total <- total + i
    total

let countdown n =
    let mutable i = n
    let mutable total = 0
    while i > 0 do
        total <- total + i
        i <- i - 1
    total

let down n =
    let mutable total = 0
    for i = n downto 1 do
        total <- total + i
    total

let edge lo hi =
    let mutable count = 0
    for i = lo to hi do
        count <- count + 1
    count

let edgeDown lo hi =
    let mutable count = 0
    for i = lo downto hi do
        count <- count + 1
    count

let rangeSum lo hi =
    let mutable total = 123
    for i = lo to hi do
        total <- total + i
    total

let rangeSumDown lo hi =
    let mutable total = 123
    for i = lo downto hi do
        total <- i + total
    total
`, flags);
        assert.equal(loops.sum(100), 5050);
        assert.equal(loops.sum(-1), 0);
        assert.equal(loops.countdown(100), 5050);
        assert.equal(loops.down(100), 5050);
        assert.equal(loops.edge(2147483647, 2147483647), 1);
        assert.equal(loops.edgeDown(-2147483648, -2147483648), 1);
        for (const [lo, hi] of [[-100, 100], [0, -1], [-2147483648, -2147483645], [2147483644, 2147483647]]) {
            const expected = lo > hi ? 123 : Number(BigInt.asIntN(32, 123n + (BigInt(hi) - BigInt(lo) + 1n) * (BigInt(lo) + BigInt(hi)) / 2n));
            assert.equal(loops.rangeSum(lo, hi), expected);
            assert.equal(loops.rangeSumDown(hi, lo), expected);
        }
        if (!flags.length) {
            for (const [lo, hi] of [[-2147483648, 2147483647], [1, 2147483647], [-2147483648, -1]]) {
                const expected = Number(BigInt.asIntN(32, 123n + (BigInt(hi) - BigInt(lo) + 1n) * (BigInt(lo) + BigInt(hi)) / 2n));
                assert.equal(loops.rangeSum(lo, hi), expected);
                assert.equal(loops.rangeSumDown(hi, lo), expected);
            }
        }

        const { exports: control } = build(`
let rec fib n = if n < 2 then n else fib (n - 1) + fib (n - 2)
let rec even n = if n = 0 then true else odd (n - 1)
and odd n = if n = 0 then false else even (n - 1)
let classify n =
    match n with
    | 0 -> 10
    | 1 | 2 -> 20
    | x when x < 0 -> 30
    | _ -> 40
let boolMatch b =
    match b with
    | true -> 42
    | false -> 0
let nested n =
    if n < 0 then
        -1
    elif n = 0 then
        0
    else
        1
let inlineLet a = let b = a + 1 in b * 2
let pipeline x = x |> inlineLet |> fib
`, flags);
        assert.equal(control.fib(20), 6765);
        assert.equal(control.even(30), 1);
        assert.equal(control.odd(30), 0);
        for (const [input, expected] of [[0, 10], [1, 20], [2, 20], [-1, 30], [3, 40]])
            assert.equal(control.classify(input), expected);
        assert.equal(control.boolMatch(1), 42);
        assert.equal(control.boolMatch(0), 0);
        assert.equal(control.nested(-5), -1);
        assert.equal(control.nested(0), 0);
        assert.equal(control.nested(5), 1);
        assert.equal(control.inlineLet(20), 42);
        assert.equal(control.pipeline(4), 55);

        const calls = [];
        const { exports: interop, module } = build(`
open System.Runtime.InteropServices
[<DllImport("host", EntryPoint="log")>]
extern void log(int value)
[<DllImport("host")>]
extern int pair(int x, int y)
[<DllImport("host")>]
extern int unused(int x)
let mutable state = 0
let record x =
    log x
    state <- state + x
    state
let combine x = pair(x, 2)
let shortAnd x = x > 0 && record 1 > 0
let shortOr x = x > 0 || record 2 > 0
let pairCurried x y = pair(x, y)
let pipeOrder () = record 3 |> pairCurried (record 4)
`, flags, { host: { log: x => calls.push(x), pair: (x, y) => x * 100 + y } });
        assert.equal(interop.record(5), 5);
        assert.equal(interop.combine(40), 4002);
        assert.equal(interop.shortAnd(0), 0);
        assert.equal(interop.shortOr(1), 1);
        assert.deepEqual(calls, [5]);
        assert.equal(interop.pipeOrder(), 1208);
        assert.deepEqual(calls, [5, 3, 4]);
        assert.equal(module instanceof WebAssembly.Module, true);
        assert.equal(WebAssembly.Module.imports(module).some(x => x.name === 'unused'), false);

        const { exports: math } = build(`
let absolute x = abs x
let absWide (x: int64) = abs x
let squareRoot x = sqrt x
let minOf a b = min a b
let maxOf a b = max a b
let toInt (x: float) = int x
let toWide (x: int) = int64 x
let toFloat (x: int64) = float x
let remainder a b = a % b
let remainderWide (a: int64) (b: int64) = a % b
`, flags);
        assert.equal(math.absolute(-42), 42);
        assert.throws(() => math.absolute(-2147483648), WebAssembly.RuntimeError);
        assert.equal(math.absWide(-42n), 42n);
        assert.throws(() => math.absWide(-9223372036854775808n), WebAssembly.RuntimeError);
        assert.equal(math.squareRoot(1764), 42);
        assert.equal(math.minOf(42, 43), 42);
        assert.equal(math.maxOf(41, 42), 42);
        assert.equal(math.toInt(42.9), 42);
        assert.equal(math.toInt(NaN), 0);
        assert.equal(math.toWide(-42), -42n);
        assert.equal(math.toFloat(42n), 42);
        assert.equal(math.remainder(42, -1), 0);
        assert.throws(() => math.remainder(-2147483648, -1), WebAssembly.RuntimeError);
        assert.throws(() => math.remainderWide(-9223372036854775808n, -1n), WebAssembly.RuntimeError);
        assert.throws(() => math.remainder(1, 0), WebAssembly.RuntimeError);

        const { exports: memory } = build(`
let greeting () = "Hello, 日本語 😀"
let quoted () = @"a""b\\c"
let escapes () = "\\u0100\\U0001F600\\000"
let append (a: string) (b: string) = a + b
let equal (a: string) (b: string) = a = b
let before (a: string) (b: string) = a < b
let matchText (s: string) =
    match s with
    | "Hello, 日本語 😀" -> 42
    | _ -> 0
let length (s: string) = s.Length
let low () = "\\u00ff"
let high () = "\\u0100"
let make n = Array.zeroCreate<int> n
let filled n = Array.create n 42
let wideArray n = Array.create n 9007199254740993L
let floats n = Array.create n 1.25
let bools n = Array.create n true
let numbers () = [| 1; 2; 3 |]
let empty () = ([||] : int array)
let sum (values: int array) =
    let mutable total = 0
    for i = 0 to values.Length - 1 do
        total <- total + values.[i]
    total
let arrayLength values = Array.length values
let setAt (values: int array) i value = values.[i] <- value
let at (values: int array) i = values.[i]
let atWide (values: int64 array) i = values.[i]
let atFloat (values: float array) i = values.[i]
let atBool values i = not values.[i]
`, flags);
        const text = pointer => {
            const length = new DataView(memory.memory.buffer).getUint32(pointer, true);
            return new TextDecoder('utf-16le', { ignoreBOM: true }).decode(new Uint8Array(memory.memory.buffer, pointer + 8, length * 2));
        };
        const greeting = memory.greeting();
        assert.equal(text(greeting), 'Hello, 日本語 😀');
        assert.equal(memory.length(greeting), 'Hello, 日本語 😀'.length);
        assert.equal(memory.matchText(greeting), 42);
        assert.equal(memory.matchText(memory.append(greeting, greeting)), 0);
        assert.equal(text(memory.quoted()), 'a"b\\c');
        assert.equal(text(memory.escapes()), '\u0100😀\0');
        const joined = memory.append(greeting, greeting);
        assert.equal(text(joined), 'Hello, 日本語 😀Hello, 日本語 😀');
        assert.equal(memory.equal(joined, memory.append(greeting, greeting)), 1);
        assert.equal(memory.equal(greeting, joined), 0);
        assert.equal(memory.before(memory.low(), memory.high()), 1);
        const values = memory.numbers();
        assert.equal(memory.sum(values), 6);
        assert.equal(memory.arrayLength(values), 3);
        memory.setAt(values, 1, 40);
        assert.equal(memory.sum(values), 44);
        assert.equal(memory.at(values, 1), 40);
        for (const index of [-1, 3, 0x40000000, 2147483647])
            assert.throws(() => memory.at(values, index), WebAssembly.RuntimeError);
        assert.throws(() => memory.setAt(values, -1, 0), WebAssembly.RuntimeError);
        const empty = memory.empty();
        assert.equal(memory.arrayLength(empty), 0);
        assert.throws(() => memory.at(empty, 0), WebAssembly.RuntimeError);
        assert.equal(memory.sum(memory.filled(10)), 420);
        assert.equal(memory.atWide(memory.wideArray(2), 1), 9007199254740993n);
        assert.equal(memory.atFloat(memory.floats(2), 1), 1.25);
        assert.equal(memory.atBool(memory.bools(2), 1), 0);
        const zeros = memory.make(20_000);
        assert.ok(memory.memory.buffer.byteLength >= 2 * 65536);
        assert.equal(memory.sum(zeros), 0);
        assert.equal(text(greeting), 'Hello, 日本語 😀');
        assert.throws(() => memory.make(-1), WebAssembly.RuntimeError);
        assert.throws(() => memory.make(2147483647), WebAssembly.RuntimeError);
        for (const pointer of [0, 4, 9, -1, 2147483647])
            assert.throws(() => memory.sum(pointer), WebAssembly.RuntimeError);
        assert.throws(() => memory.sum(greeting), WebAssembly.RuntimeError);
        const header = new DataView(memory.memory.buffer);
        const savedLength = header.getUint32(values, true);
        header.setUint32(values, 0x7fffffff, true);
        assert.throws(() => memory.at(values, 0), WebAssembly.RuntimeError);
        header.setUint32(values, savedLength, true);
        assert.equal(memory.at(values, 1), 40);

        const { exports: references } = build(`
let mutable saved = "initial"
let text () = "x"
let rec tailText (s: string) n =
    if n <= 0 then s.Length
    else tailText saved (n - 1)
let save (s: string) = saved <- s + "!"
`, flags);
        assert.equal(references.tailText(references.text(), 1), 7);
        references.saved.value = 0;
        assert.throws(() => references.tailText(references.text(), 1), WebAssembly.RuntimeError);
        references.save(references.text());
        const retainedString = references.saved.value;
        references.save(references.text());
        assert.notEqual(references.saved.value, retainedString);
        assert.deepEqual([...new Uint16Array(references.memory.buffer, retainedString + 8, 2)], [120, 33]);
    }

    const { exports: capped } = build('let make n = Array.zeroCreate<int> n', ['--max-memory-pages', '1']);
    assert.equal(capped.memory.buffer.byteLength, 65536);
    assert.throws(() => capped.make(16381), WebAssembly.RuntimeError);
    assert.equal(capped.make(16380), 8);
    assert.throws(() => capped.make(0), WebAssembly.RuntimeError);
    assert.equal(capped.memory.buffer.byteLength, 65536);

    for (const file of ['math.fs', 'arrays.fs']) {
        const source = readFileSync(fileURLToPath(new URL(`../examples/${file}`, import.meta.url)), 'utf8');
        build(source);
    }
    let browser;
    let rendered;
    const browserSource = readFileSync(fileURLToPath(new URL('../examples/browser/app.fs', import.meta.url)), 'utf8');
    const browserText = pointer => {
        const buffer = browser.exports.memory.buffer;
        const length = new DataView(buffer).getUint32(pointer, true);
        return new TextDecoder('utf-16le', { ignoreBOM: true }).decode(new Uint8Array(buffer, pointer + 8, length * 2));
    };
    browser = build(browserSource, [], { dom: {
        readInput: id => { assert.equal(browserText(id), 'count'); return 20; },
        showResult: (id, value) => { assert.equal(browserText(id), 'result'); rendered = value; },
    } });
    browser.exports.render();
    assert.equal(rendered, 6765);
    const emptyImport = build('[<DllImport("host", EntryPoint="")>]\nextern int empty()\nlet run () = empty ()\n',
        [], { host: { '': () => 42 } });
    assert.equal(emptyImport.exports.run(), 42);
    const verbatimImport = build('[<DllImport(@"host\\module", EntryPoint=@"a""b")>]\nextern int call()\nlet run () = call ()\n',
        [], { 'host\\module': { 'a"b': () => 42 } });
    assert.equal(verbatimImport.exports.run(), 42);

    const { exports: tail } = build(`
let rec sum n acc = if n <= 0 then acc else sum (n - 1) (acc + n)
let rec swap n a b = if n = 0 then a - b else swap (n - 1) b a
let rec triangle n = if n <= 0 then 0 else n + triangle (n - 1)
let rec wideTriangle n = if n <= 0 then 0L else int64 n + wideTriangle (n - 1)
`);
    assert.equal(tail.sum(1_000_000, 0), 1784293664);
    assert.equal(tail.swap(1_000_001, 42, 1), -41);
    assert.equal(tail.triangle(1_000_000), 1784293664);
    assert.equal(tail.wideTriangle(1_000_000), 500000500000n);

    const { exports: regions } = build(`
let scratch n =
    let values = Array.zeroCreate<int> n
    values.[0] <- 42
    values.[0]
let rec scratchLoop n acc =
    if n <= 0 then acc
    else
        let values = Array.create 64 n
        scratchLoop (n - 1) (acc + values.[0])
`, ['--max-memory-pages', '1']);
    for (let i = 0; i < 1000; i++) assert.equal(regions.scratch(1000), 42);
    assert.equal(regions.scratchLoop(100_000, 0), 705082704);
    assert.equal(regions.memory.buffer.byteLength, 65536);

    const retained = [];
    const { exports: escaping } = build(`
open System.Runtime.InteropServices
[<DllImport("host")>]
extern void retain(int[] values)
let publish n = retain (Array.create n 42)
`, ['--max-memory-pages', '1'], { host: { retain: pointer => retained.push(pointer) } });
    escaping.publish(4);
    escaping.publish(4);
    assert.notEqual(retained[0], retained[1]);
    for (const pointer of retained)
        assert.deepEqual([...new Int32Array(escaping.memory.buffer, pointer + 8, 4)], [42, 42, 42, 42]);

    const observed = [];
    const { exports: sideEffects } = build(`
open System.Runtime.InteropServices
[<DllImport("host")>]
extern int note(int value)
let rec annotated n = if n <= 0 then 0 else note n + annotated (n - 1)
let storeWithEffect (values: int array) index = values.[index] <- note 42
let make () = Array.zeroCreate<int> 1
`, [], { host: { note: value => { observed.push(value); return value; } } });
    assert.equal(sideEffects.annotated(4), 10);
    assert.deepEqual(observed, [4, 3, 2, 1]);
    assert.throws(() => sideEffects.storeWithEffect(sideEffects.make(), 1), WebAssembly.RuntimeError);
    assert.deepEqual(observed, [4, 3, 2, 1, 42]);

    let seed = 0x465357;
    const random = limit => {
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        return limit === undefined ? seed | 0 : seed % limit;
    };
    function tree(depth) {
        if (!depth || random(4) === 0) {
            const leaves = [
                ['x', (x, y) => x], ['y', (x, y) => y],
                ['2147483647', () => 2147483647], ['-2147483648', () => -2147483648],
            ];
            return leaves[random(leaves.length)];
        }
        const [a, evaluateA] = tree(depth - 1), [b, evaluateB] = tree(depth - 1);
        if (random(5) === 0)
            return [`(if x < y then ${a} else ${b})`, (x, y) => x < y ? evaluateA(x, y) : evaluateB(x, y)];
        const operations = [
            ['+', (x, y) => (x + y) | 0], ['-', (x, y) => (x - y) | 0],
            ['*', (x, y) => Math.imul(x, y)], ['&&&', (x, y) => x & y],
            ['|||', (x, y) => x | y], ['^^^', (x, y) => x ^ y],
            ['<<<', (x, y) => x << (y & 31)], ['>>>', (x, y) => x >> (y & 31)],
        ];
        const [op, evaluate] = operations[random(operations.length)];
        return [`(${a} ${op} ${b})`, (x, y) => evaluate(evaluateA(x, y), evaluateB(x, y))];
    }
    const generated = Array.from({ length: 128 }, () => tree(5));
    const generatedSource = generated.map(([source], i) => `let generated${i} (x: int) (y: int) = ${source}`).join('\n');
    const inputs = Array.from({ length: 32 }, () => [random(), random()]);
    for (const flags of [[], ['--no-opt']]) {
        const { exports } = build(generatedSource, flags);
        generated.forEach(([source, evaluate], i) => {
            for (const [x, y] of inputs)
                assert.equal(exports[`generated${i}`](x, y), evaluate(x, y), `generated mismatch: ${source}`);
        });
    }
    const manyReads = build('let reads (values: int array) =\n' +
        '    ignore values.[0]\n'.repeat(2000) + '    ()\nlet make () = [|42|]\n');
    assert.equal(manyReads.exports.reads(manyReads.exports.make()), undefined);

    const cliSource = join(directory, 'space 日本語.fs');
    const cliOutput = join(directory, 'space 日本語.wasm');
    const cliText = 'let answer () = 42\n';
    writeFileSync(cliSource, cliText);
    let cli = spawnSync(compiler, ['check', cliSource, '--stats'], { encoding: 'utf8' });
    assert.equal(cli.status, 0, cli.stderr);
    assert.equal(cli.stdout, '');
    const statistics = JSON.parse(cli.stderr);
    assert.equal(statistics.source_bytes, Buffer.byteLength(cliText));
    assert.ok(statistics.wasm_bytes > 8);
    assert.ok(statistics.compile_ms >= 0);
    assert.equal(existsSync(cliOutput), false);
    cli = spawnSync(compiler, [cliSource, '-o', cliOutput], { encoding: 'utf8' });
    assert.equal(cli.status, 0, cli.stderr);
    assert.ok(WebAssembly.validate(readFileSync(cliOutput)));
    const hardlink = join(directory, 'source-alias.fs');
    linkSync(cliSource, hardlink);
    for (const destination of [cliSource, hardlink]) {
        cli = spawnSync(compiler, [cliSource, '-o', destination], { encoding: 'utf8' });
        assert.equal(cli.status, 1);
        assert.match(cli.stderr, /must not overwrite/);
        assert.equal(readFileSync(cliSource, 'utf8'), cliText);
    }
    cli = spawnSync(compiler, [cliSource, '-o', directory], { encoding: 'utf8' });
    assert.equal(cli.status, 1);
    assert.match(cli.stderr, /cannot replace output/);
    assert.equal(readdirSync(directory).some(name => name.startsWith('.fsw-')), false);
    assert.equal(spawnSync(compiler, ['--help']).status, 0);
    assert.equal(spawnSync(compiler, []).status, 2);
    assert.equal(spawnSync(compiler, ['check', cliSource, '-o', cliOutput]).status, 1);

    const source = 'let private unused x = x + 9\nlet add a b = a + b\n';
    const first = build(source);
    const second = build(source);
    assert.deepEqual(first.bytes, second.bytes);
    assert.deepEqual(Object.keys(first.exports), ['add']);
    assert.ok(first.bytes.length <= 48, `minimal addition module is ${first.bytes.length} bytes`);
    assert.deepEqual(Object.keys(build('let a x = x + 1\nlet b x = a x + 1', ['--export', 'b']).exports), ['b']);
    assert.equal(build('\ufeff(* outer (* nested *) *)\r\nlet ``answer value`` () = 42\r\n').exports['answer value'](), 42);
    assert.equal(build('module Quoted\nlet ``Quoted.value`` () = 42\nlet value () = 1\nlet run () = ``Quoted.value`` () + Quoted.value ()\n').exports.run(), 43);

    rejects('let bad x = x + true', /type mismatch/);
    rejects('let bad () = missing 1', /undefined function/);
    rejects('let f n = f n', /let rec/);
    rejects('let f x = g x\nlet g x = x + 1', /earlier definition/);
    rejects('let f () = 2147483648', /out-of-range integer/);
    rejects('let f () = 9223372036854775808L', /out-of-range integer/);
    rejects('let f () = 1e999', /out-of-range float/);
    rejects('let f () =\n    let x = 1\n    x <- 2\n    x', /immutable/);
    rejects('let f () =\n\t42', /tabs/);
    rejects('let f x = match x with | 1 -> 2', /non-exhaustive/);
    rejects('let f x = x', /ambiguous type/);
    rejects('let f x = if x then 1 else 2.0', /type mismatch/);
    rejects('let f () =\n    let x = 1', /followed by an expression/);
    rejects('let f () = 1\nlet f () = 2', /duplicate declaration/);
    rejects('let f () = 1 let g () = 2', /separate, unindented lines/);
    rejects('let class = 1', /non-keyword/);
    rejects('let f (x: void) = ()', /extern return type/);
    rejects('let f () = 1', /unknown export/, ['--export', 'missing']);
    rejects('let f () = 1', /unknown option/, ['--wat']);
    rejects('let f () = (* missing', /unterminated block comment/);
    rejects('let f () = (1 + 2]', /mismatched/);
    rejects('let f () = "\\u00xz"', /invalid Unicode escape/);
    rejects('let f () = "\\q"', /invalid string escape/);
    rejects('let f () = [| 1; true |]', /type mismatch/);
    rejects('let f (a: float array) = a.[true]', /type mismatch/);
    rejects('let f () = 1', /maximum memory/, ['--max-memory-pages', '0']);
    rejects('let f () = 1', /maximum memory/, ['--max-memory-pages', '32768']);
    rejects('let f () = 1', /maximum memory/, ['--max-memory-pages', '10oops']);
    rejects('let memory () = "x"', /reserved/);
    rejects(Buffer.from([0xff]), /invalid UTF-8/);
    rejects('let f () = 1\0', /NUL/);
    rejects(`[<DllImport("host")>]\nextern int f(${Array.from({ length: 257 }, (_, i) => `int p${i}`).join(', ')})`, /too many parameters/);
    rejects('let f () = missing ' + '1 '.repeat(257), /too many function arguments/);
    rejects(`let f () = ${'('.repeat(300)}1${')'.repeat(300)}`, /deeply nested/);
    rejects(`let f () = ${'1 + '.repeat(300)}1`, /deeply nested/);

    console.log(`OK: ${count} compilation checks plus Wasm execution assertions`);
} finally {
    rmSync(directory, { recursive: true, force: true });
}
