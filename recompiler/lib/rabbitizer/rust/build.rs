fn main() {
    let c_paths: Vec<std::path::PathBuf> = glob::glob("src/**/*.c")
        .unwrap()
        .filter_map(|g| g.ok())
        .collect();
    let h_paths: Vec<std::path::PathBuf> = glob::glob("include/**/*.h")
        .unwrap()
        .filter_map(|g| g.ok())
        .collect();
    let tables_paths: Vec<std::path::PathBuf> = glob::glob("tables/tables/**/*.inc")
        .unwrap()
        .filter_map(|g| g.ok())
        .collect();

    for path in c_paths.iter().chain(&h_paths).chain(&tables_paths) {
        println!("cargo:rerun-if-changed={}", path.to_string_lossy());
    }

    assert!(!c_paths.is_empty());

    cc::Build::new()
        .files(c_paths)
        .include("include")
        .include("tables")
        .warnings(false)
        .compile("rabbitizer");
}
