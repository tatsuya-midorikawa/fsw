# fsw — FSharp to Wasm

F# の計算用サブセットを WebAssembly に直接コンパイルする、実験的なネイティブコンパイラです。
実装は C++17 標準ライブラリと最小限の OS API のみ。OS・標準ライブラリ以外をリンクせず、
コンパイル時に LLVM IR、リンカー、.NET、パッケージマネージャーを起動しません。

```fsharp
module Sample

let add a b = a + b

let rec sum n acc =
    if n <= 0 then acc
    else sum (n - 1) (acc + n)
```

加算関数だけの最小例は **41 バイトの Wasm** になります。
数値だけのモジュールには線形メモリや補助 JavaScript ランタイムを付けません。

**現在は `0.1.0-dev` です。F# 全体・.NET・汎用 GC は未実装であり、
あらゆる用途で最速・最小だと主張するものではありません。**
対応範囲と ABI は [言語仕様](docs/language.md)、測定方法と結果は
[ベンチマーク](docs/benchmarks.md) を参照してください。

## ビルド

必要なのは C++17 コンパイラと CMake 3.20 以降です。
通常のビルドでは依存パッケージをダウンロードしません。
テストには Node.js 20 以降、F# との照合には追加で .NET 10 SDK を使用します。
.NET は fsw のビルド・実行自体には不要です。

### macOS

Xcode Command Line Tools と CMake を用意します。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel 2
./build/fsw --version
./build/fsw examples/math.fs -o build/math.wasm
```

CMake を使わない最小のビルドも可能です。

```sh
mkdir -p build
c++ -std=c++17 -O2 -DNDEBUG src/main.cpp src/frontend.cpp src/check.cpp src/wasm.cpp -o build/fsw
```

### Windows

Visual Studio 2022 の「C++ によるデスクトップ開発」と CMake を用意し、
Developer PowerShell から実行します。

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel 2
.\build\Release\fsw.exe --version
.\build\Release\fsw.exe examples\math.fs -o build\math.wasm
```

MSVC では C/C++ ランタイムを静的リンクします。UTF-8 ソース、CRLF、
日本語・空白を含む入出力パスを扱える構成です。
別の CMake ジェネレーターでは実行ファイルの配置先が異なる場合があります。

macOS arm64 のローカル検証に加え、Windows Server 2022、macOS 14、Ubuntu 24.04 の
CI でビルド・Wasm 実行・F# 照合を確認しています。Linux ではサニタイザー検証も実行します。

## 実行

生成した Wasm は標準の WebAssembly API で読み込めます。
例えば Node.js では次のコマンドで `6765` を出力します。

```sh
node -e "WebAssembly.instantiate(require('node:fs').readFileSync('build/math.wasm')).then(({instance}) => console.log(instance.exports.Fibonacci(20)))"
```

ブラウザー用の例は `examples/browser/` にあります。

`examples/functional.fs` にはラムダ、`Array.map`、`Array.fold`、`Array.iter` の例もあります。
これらの配列処理は型付きのループへ変換し、クロージャー用ランタイムを追加しません。

```sh
./build/fsw examples/browser/app.fs -o examples/browser/app.wasm
python3 -m http.server 8000 --bind 127.0.0.1 --directory examples/browser
```

Windows ではコンパイラを `.\build\Release\fsw.exe`、
Python ランチャーを `py -3 -m http.server ...` に置き換えます。
Python はこの静的ファイル配信例にだけ使用します。
`http://127.0.0.1:8000` を開くと、F# からホスト関数経由で表示を更新します。

## CLI

```text
fsw [build|check] source.fs [-o output.wasm] [options]
```

| オプション | 動作 |
|---|---|
| `-o`, `--output PATH` | 出力先。省略時は入力拡張子を `.wasm` に変更 |
| `check` | 構文・型・生成処理を検査。ファイルを出力しない |
| `--export NAME` | 指定した宣言だけを公開。複数回指定可能 |
| `--no-opt` | 定数畳み込み・末尾再帰などの最適化を無効化 |
| `--max-memory-pages N` | 線形メモリ上限。1 ページは 64 KiB、既定は 256 |
| `--stats` | 標準エラーに JSON の処理時間・サイズ・関数数を出力 |
| `--version`, `--help` | バージョン・使用方法 |
| `--` | 以降をオプションではなく入力パスとして扱う |

入力は 1 ファイルです。`int64` のホスト引数・戻り値には JavaScript の `BigInt` を使います。
インポート関数の引数・戻り値も含む詳細は [ABI](docs/language.md#公開-abi) を参照してください。

エラーは `file:line:column: error: ...` の形で出力します。
成功は終了コード 0、診断・I/O エラーは 1、引数なしの呼び出しは 2 です。
コンパイル失敗では既存の出力を変更しません。出力は同一ファイルシステムの一時ファイルから
置換し、入力ファイルやそのハードリンクを出力で上書きしないよう検査します。

## 検証と開発

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
node tests/differential.mjs
```

スクリプトの既定の実行ファイルは macOS / Linux では `build/fsw`、
Windows では `build/Release/fsw.exe` です。別の配置先は第 1 引数に指定できます。
`tests/differential.mjs` だけが .NET 10 SDK を必要とします。

検証内容は、実際の Wasm 検証・実行、整数式の生成テスト、F# の値と例外動作の照合、
境界値、評価順序、メモリ寿命・上限、不正 UTF-8、診断、出力保護、再現性です。
サニタイザー付きのビルドと、標準ライブラリだけの決定的な入力変異テストも CI に含めています。

```sh
c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DFSW_STANDALONE_FUZZ src/frontend.cpp src/check.cpp src/wasm.cpp tests/fuzz.cpp -o build/fsw-fuzz
build/fsw-fuzz examples/math.fs examples/arrays.fs examples/functional.fs examples/browser/app.fs
```

このサニタイザー用コマンドは対応する Clang / GCC 環境用です。
CI は macOS、Windows、Linux で通常のテストを、Linux でサニタイザー検証を実行する構成です。
Actions は GitHub 所有のものをコミット SHA で固定し、ジョブのリポジトリ権限は読み取りのみにしています。

## 依存と構成

製品には OS・標準ライブラリ以外の依存、パッケージ解決、生成時のソースコード実行、
自動更新、テレメトリーはありません。
標準ライブラリ・OS・コンパイラ・ビルド／テストツールへの信頼まで不要になるわけではありません。

| ファイル | 役割 |
|---|---|
| `src/frontend.cpp` | UTF-8・字句・レイアウト解析、構文解析、型制約 |
| `src/check.cpp` | 名前解決、単相型推論、定数評価 |
| `src/wasm.cpp` | 生の Wasm 生成、必要な場合だけメモリ補助関数を生成 |
| `src/main.cpp` | CLI、入出力、原子的な出力置換 |
| `tests/` | Node.js / F# の実行照合、ネイティブ変異テスト |

ライセンスは MIT です。