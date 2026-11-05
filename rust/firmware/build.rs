include!("src/config_names.rs");

fn main() {
    embuild::espidf::sysenv::output();
    for name in NAMES {
        println!("cargo:rerun-if-env-changed={name}");
    }
}
