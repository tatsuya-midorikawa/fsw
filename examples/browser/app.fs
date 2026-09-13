module Browser

open System.Runtime.InteropServices

[<DllImport("dom")>]
extern int readInput(string id)

[<DllImport("dom")>]
extern void showResult(string id, int value)

let rec private fibonacci n a b =
    if n <= 0 then a
    else fibonacci (n - 1) b (a + b)

let render () =
    let value = readInput "count"
    showResult("result", fibonacci value 0 1)
