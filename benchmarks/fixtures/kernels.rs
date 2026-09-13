#![no_std]
#![no_main]

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

#[no_mangle]
pub extern "C" fn Add(a: i32, b: i32) -> i32 {
    a.wrapping_add(b)
}

#[no_mangle]
pub extern "C" fn Fibonacci(n: i32) -> i32 {
    if n < 2 { n } else { Fibonacci(n - 1).wrapping_add(Fibonacci(n - 2)) }
}

#[no_mangle]
pub extern "C" fn Checksum(n: i32, seed: i32) -> i32 {
    let mut value = seed;
    for i in 0..n {
        value = (value ^ (value << 5)).wrapping_add(i ^ (value >> 2));
    }
    value
}
