package main

//go:wasmexport Add
func Add(a, b int32) int32 { return a + b }

func main() {}
