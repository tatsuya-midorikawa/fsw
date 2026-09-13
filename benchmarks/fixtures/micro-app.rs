#![no_std]
#![no_main]

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

#[link(wasm_import_module = "env")]
extern "C" {
    fn set_text(id: i32, data: *const u8, length: i32);
    fn set_number(id: i32, value: i32);
}

fn fibonacci(n: i32) -> i32 {
    if n < 2 { n } else { fibonacci(n - 1).wrapping_add(fibonacci(n - 2)) }
}

#[no_mangle]
pub extern "C" fn InitApp() {
    let status = "Ready (WebAssembly)";
    unsafe {
        set_text(0, status.as_ptr(), status.len() as i32);
        set_number(1, 0);
    }
}

#[no_mangle]
pub extern "C" fn RunComputation(n: i32) -> i32 { fibonacci(n) }

#[no_mangle]
pub extern "C" fn Render(n: i32) -> i32 {
    let result = fibonacci(n);
    let message = if n % 2 == 0 { "Even: café / 日本語 / 😀" } else { "Odd: café / 日本語 / 😀" };
    unsafe {
        set_text(0, message.as_ptr(), message.len() as i32);
        set_number(1, result);
    }
    result
}
