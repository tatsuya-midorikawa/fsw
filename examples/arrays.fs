module Arrays

let create count = Array.zeroCreate<float> count

let set (values: float array) index value =
    values.[index] <- value

let sum (values: float array) =
    let mutable total = 0.0
    for i = 0 to values.Length - 1 do
        total <- total + values.[i]
    total

let dot (left: float array) (right: float array) =
    let mutable total = 0.0
    for i = 0 to min left.Length right.Length - 1 do
        total <- total + left.[i] * right.[i]
    total
