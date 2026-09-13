package main

//go:wasmexport Add
func Add(a, b int32) int32 { return a + b }

//go:wasmexport Fibonacci
func Fibonacci(n int32) int32 {
	if n < 2 {
		return n
	}
	return Fibonacci(n-1) + Fibonacci(n-2)
}

//go:wasmexport Checksum
func Checksum(n, seed int32) int32 {
	value := seed
	for i := int32(0); i < n; i++ {
		value = (value ^ (value << 5)) + (i ^ (value >> 2))
	}
	return value
}

func main() {}
