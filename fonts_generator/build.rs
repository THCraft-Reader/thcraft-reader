use std::{env, path::PathBuf, process::Command};

fn main() {
    println!("cargo:rerun-if-changed=native/font_engine.h");
    println!("cargo:rerun-if-changed=native/font_engine.cpp");
    println!("cargo:rerun-if-env-changed=FG_NATIVE_LIB_DIR");
    println!("cargo:rerun-if-env-changed=FG_EMSCRIPTEN_VERSION");
    let rustc = env::var_os("RUSTC").unwrap_or_else(|| "rustc".into());
    let version = Command::new(rustc)
        .arg("--version")
        .output()
        .ok()
        .filter(|result| result.status.success())
        .map(|result| String::from_utf8_lossy(&result.stdout).trim().to_owned())
        .unwrap_or_else(|| "unknown".to_owned());
    println!("cargo:rustc-env=FG_RUST_VERSION={version}");
    let emscripten = env::var("FG_EMSCRIPTEN_VERSION")
        .unwrap_or_else(|_| "not linked (native unit tests)".to_owned());
    println!("cargo:rustc-env=FG_EMSCRIPTEN_VERSION={emscripten}");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("emscripten") {
        // Pure Rust table/serialization unit tests do not call the FFI bridge.
        return;
    }
    let directory = env::var_os("FG_NATIVE_LIB_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("build/native/lib")
        });
    for library in ["font_engine", "freetype", "harfbuzz"] {
        let archive = directory.join(format!("lib{library}.a"));
        assert!(
            archive.is_file(),
            "{} is missing; run python build.py",
            archive.display()
        );
        println!("cargo:rerun-if-changed={}", archive.display());
        println!("cargo:rustc-link-lib=static={library}");
    }
    println!("cargo:rustc-link-search=native={}", directory.display());
}
