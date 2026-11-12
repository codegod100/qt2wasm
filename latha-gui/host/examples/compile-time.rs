// Times compiling a component with latha-gui's engine settings (and its function cache), without the
// whole-component cache: `cargo run --release --example compile-time APP.wasm`.
fn main() -> wasmtime::Result<()> {
    let path = std::env::args().nth(1).expect("usage: compile-time APP.wasm");
    let bytes = std::fs::read(&path)?;
    let mut config = latha_host::procs::base_config();
    config.wasm_component_model(true);
    let engine = wasmtime::Engine::new(&config)?;
    let t = std::time::Instant::now();
    let _c = wasmtime::component::Component::new(&engine, &bytes)?;
    println!("{path}: compiled in {:.1} s", t.elapsed().as_secs_f64());
    Ok(())
}
