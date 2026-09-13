module Math

let Add a b = a + b

let rec Fibonacci n =
    if n < 2 then n
    else Fibonacci (n - 1) + Fibonacci (n - 2)

let Sum n =
    let mutable total = 0
    for i = 1 to n do
        total <- total + i
    total

let rec private fibonacciLoop n a b =
    if n <= 0 then a
    else fibonacciLoop (n - 1) b (a + b)

let FibonacciFast n = fibonacciLoop n 0 1
