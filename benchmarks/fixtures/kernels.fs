module Benchmark

let Add a b = a + b

let rec Fibonacci n =
    if n < 2 then n
    else Fibonacci (n - 1) + Fibonacci (n - 2)

let Checksum n seed =
    if n <= 0 then seed
    else
        let mutable value = seed
        for i = 0 to n - 1 do
            value <- (value ^^^ (value <<< 5)) + (i ^^^ (value >>> 2))
        value
