//! `cargo bindings`: writes `sys/src/<chip>.rs` for every chip with a `include/<chip>/sdkconfig.h`,
//! from the same ESP-IDF headers the C firmware compiles against.

use std::{
    env, fs,
    path::{Path, PathBuf},
};

/// The ESP-IDF headers declaring what the kernel implements.
const HEADERS: &[&str] = &[
    "freertos/FreeRTOS.h",
    "freertos/task.h",
    "freertos/queue.h",
    "esp_err.h",
    "esp_log.h",
    "esp_timer.h",
    "esp_adc/adc_oneshot.h",
    "esp_adc/adc_cali_scheme.h",
];

/// The C functions the kernel implements.
const FUNCTIONS: &[&str] = &[
    "xTaskCreatePinnedToCore",
    "vTaskDelay",
    "xQueueGenericCreate",
    "xQueueGenericReset",
    "xQueueGenericSend",
    "xQueueReceive",
    "vPortEnterCritical",
    "vPortExitCritical",
    "esp_log",
    "esp_log_timestamp",
    "esp_timer_get_time",
    "_esp_error_check_failed",
    "adc_oneshot_new_unit",
    "adc_oneshot_config_channel",
    "adc_oneshot_read",
    "adc_cali_create_scheme_curve_fitting",
    "adc_cali_raw_to_voltage",
];

/// The C constants the kernel uses.
const CONSTANTS: &[&str] = &[
    "ESP_OK",
    "ESP_ERR_.*",
    "pdPASS",
    "pdFAIL",
    "pdTRUE",
    "pdFALSE",
    "errQUEUE_FULL",
    "queueSEND_TO_.*",
    "queueQUEUE_TYPE_BASE",
    "CONFIG_LOG_DEFAULT_LEVEL",
    "tskNO_AFFINITY",
    "configTICK_RATE_HZ",
];

fn main() {
    let kernel = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("bindings/ is inside kernel/");
    let platformio = env::var_os("PLATFORMIO_CORE_DIR").map_or_else(
        || PathBuf::from(env::var_os("HOME").expect("HOME is set")).join(".platformio"),
        PathBuf::from,
    );
    let idf = platformio.join("packages/framework-espidf/components");
    let newlib = platformio.join("packages/toolchain-riscv32-esp/riscv32-esp-elf/include");
    let version = fs::read_to_string(platformio.join("packages/framework-espidf/version.txt"))
        .expect("framework-espidf is installed");
    let ini =
        fs::read_to_string(kernel.join("pio/kernel.ini")).expect("kernel/pio/kernel.ini exists");
    // A file, not `header_contents`: bindgen evaluates macros with casts (`pdPASS`, ...) only then.
    let wrapper = env::temp_dir().join("baja-kernel-bindings.h");
    let includes: String = HEADERS
        .iter()
        .map(|header| format!("#include \"{header}\"\n"))
        .collect();
    fs::write(&wrapper, includes).expect("the temporary folder is writable");

    for chip in ["esp32c3", "esp32p4"] {
        let sdkconfig = kernel.join("include").join(chip);
        if !sdkconfig.join("sdkconfig.h").exists() {
            continue;
        }
        let arch = match chip {
            "esp32c3" => ["-march=rv32imc_zicsr_zifencei", "-mabi=ilp32"],
            _ => ["-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f"],
        };
        let mut args = vec![
            "--target=riscv32-unknown-none-elf".into(),
            "-std=gnu17".into(),
            "-DESP_PLATFORM".into(),
        ];
        args.extend(arch.map(String::from));
        args.extend(include_dirs(&ini, chip).map(|dir| format!("-I{}", idf.join(dir).display())));
        args.push(format!("-I{}", sdkconfig.display()));
        args.push(format!("-isystem{}", newlib.display()));

        let mut builder = bindgen::Builder::default()
            .header(wrapper.display().to_string())
            .clang_args(args)
            .use_core()
            .generate_comments(false)
            .formatter(bindgen::Formatter::Prettyplease)
            .raw_line(format!("// ESP-IDF {}", version.trim()))
            .clang_macro_fallback()
            .clang_macro_fallback_build_dir(env::temp_dir())
            .default_macro_constant_type(bindgen::MacroTypeVariation::Signed)
            .default_enum_style(bindgen::EnumVariation::NewType {
                is_bitfield: false,
                is_global: false,
            });
        for function in FUNCTIONS {
            builder = builder.allowlist_function(function);
        }
        for constant in CONSTANTS {
            builder = builder.allowlist_var(constant);
        }
        let out = kernel.join("sys/src").join(format!("{chip}.rs"));
        builder
            .generate()
            .expect("bindgen can read the headers")
            .write_to_file(&out)
            .expect("sys/src is writable");
        println!("wrote {}", out.display());
    }
}

/// The ESP-IDF header folders `kernel.ini` gives the C firmware for `chip`, relative to `$IDF`.
fn include_dirs<'ini>(ini: &'ini str, chip: &str) -> impl Iterator<Item = &'ini str> {
    let mut section = "";
    ini.lines().map(str::trim).filter_map(move |line| {
        if let Some(name) = line
            .strip_prefix('[')
            .and_then(|rest| rest.strip_suffix(']'))
        {
            section = name;
            return None;
        }
        let ours = section == "kernel" || section == format!("kernel_{chip}");
        line.strip_prefix("-I$IDF/").filter(|_| ours)
    })
}
