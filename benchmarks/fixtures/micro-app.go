package main

import "unsafe"

//go:wasmimport env set_text
func setText(id int32, data *byte, length int32)

//go:wasmimport env set_number
func setNumber(id, value int32)

func fibonacci(n int32) int32 {
	if n < 2 {
		return n
	}
	return fibonacci(n-1) + fibonacci(n-2)
}

//go:wasmexport InitApp
func InitApp() {
	status := "Ready (WebAssembly)"
	setText(0, unsafe.StringData(status), int32(len(status)))
	setNumber(1, 0)
}

//go:wasmexport RunComputation
func RunComputation(n int32) int32 { return fibonacci(n) }

//go:wasmexport Render
func Render(n int32) int32 {
	result := fibonacci(n)
	message := "Odd: café / 日本語 / 😀"
	if n%2 == 0 {
		message = "Even: café / 日本語 / 😀"
	}
	setText(0, unsafe.StringData(message), int32(len(message)))
	setNumber(1, result)
	return result
}

func main() {}
