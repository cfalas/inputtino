extern crate bindgen;

use std::env;
use std::path::PathBuf;

use cmake::Config;

fn main() {
    let build_static = false;

    // This is the directory where the `c` library is located.
    let libdir_path = PathBuf::from("../../")
        // Canonicalize the path as `rustc-link-search` requires an absolute
        // path.
        .canonicalize()
        .expect("cannot canonicalize path");

    // Compile the library using CMake
    let dst = Config::new(libdir_path)
        .target("libinputtino")
        .define("BUILD_SHARED_LIBS", if build_static { "OFF" } else { "ON" })
        .define("LIBINPUTTINO_INSTALL", "ON")
        .define("BUILD_TESTING", "OFF")
        .define("BUILD_SERVER", "OFF")
        .define("BUILD_C_BINDINGS", "ON")
        .profile("Release")
        .define("CMAKE_CONFIGURATION_TYPES", "Release")
        .build();

    // Dependencies
    if !build_static {
        println!("cargo:rustc-link-lib=evdev");
        println!("cargo:rustc-link-lib=udev");
        println!("cargo:rustc-link-lib=stdc++");
    }

    //libinputtino
    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    println!("cargo:rustc-link-lib={}libinputtino", if build_static { "static=" } else { "" });

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        .use_core()
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: false,
        })
        // Add the include directory
        .clang_arg(format!("-I{}/include/", dst.display()))
        // Set the INPUTTINO_STATIC_DEFINE macro
        .clang_arg(if build_static {"-D INPUTTINO_STATIC_DEFINE=1"} else {""})
        // The input header we would like to generate bindings for.
        .header("wrapper.hpp")
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings
        .write_to_file(out_path)
        .expect("Couldn't write bindings!");
}
