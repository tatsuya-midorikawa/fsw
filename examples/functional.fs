module Functional

let sample () = [|1; 2; 3|]

let square = fun value -> value * value

let sumSquares (values: int array) =
    values
    |> Array.map square
    |> Array.fold (+) 0

let affine (values: float array) scale offset =
    Array.map (fun value -> value * scale + offset) values

let sumWithCapture (values: int array) =
    let mutable total = 0
    Array.iter (fun value -> total <- total + value) values
    total
