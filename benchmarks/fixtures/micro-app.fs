module Benchmark

open System.Runtime.InteropServices

[<DllImport("env")>]
extern void set_text(int id, string text, int length)

[<DllImport("env")>]
extern void set_number(int id, int value)

let rec private fibonacci n =
    if n < 2 then n
    else fibonacci (n - 1) + fibonacci (n - 2)

let InitApp () =
    let status = "Ready (WebAssembly)"
    set_text(0, status, status.Length)
    set_number(1, 0)

let RunComputation n = fibonacci n

let Render n =
    let result = fibonacci n
    let message =
        if n % 2 = 0 then "Even: café / 日本語 / 😀"
        else "Odd: café / 日本語 / 😀"
    set_text(0, message, message.Length)
    set_number(1, result)
    result
