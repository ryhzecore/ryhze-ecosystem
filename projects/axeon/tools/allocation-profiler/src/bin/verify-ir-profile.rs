#![forbid(unsafe_code)]
//! Repeated checked-IR verification timing, deliberately without a counting allocator.
use axeon::project::{Project, ProjectError};
use std::ffi::OsString;
use std::fmt::Write as FormatWrite;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

const MAX_SAMPLES: usize = 101;
const DEADLINE: Duration = Duration::from_secs(30);
const HELP: &str =
    "verify-ir-profile SOURCE --output-dir NEW_DIR [--project-root PATH] [--samples N]\n\
Repeated verification of one loaded/checked IR; 1 warmup, 1..101 measured samples (default 7).\n\
No source execution, counting allocator, end-to-end compile timing or overwrite.\n";

struct Config {
    source: PathBuf,
    output: PathBuf,
    root: Option<PathBuf>,
    samples: usize,
}
fn config(args: impl IntoIterator<Item = OsString>) -> Result<Config, String> {
    let mut args = args.into_iter();
    let source = PathBuf::from(args.next().ok_or(HELP)?);
    if source.as_os_str().is_empty() || source.as_os_str().to_string_lossy().starts_with("--") {
        return Err("SOURCE is required".into());
    }
    let (mut output, mut root, mut samples) = (None, None, None);
    while let Some(flag) = args.next() {
        let value = args.next().ok_or("option requires a value")?;
        if value.is_empty() {
            return Err("option value must not be empty".into());
        }
        match flag.to_str() {
            Some("--output-dir") if output.is_none() => output = Some(PathBuf::from(value)),
            Some("--project-root") if root.is_none() => root = Some(PathBuf::from(value)),
            Some("--samples") if samples.is_none() => {
                let value = value.to_str().ok_or("samples must be decimal digits")?;
                if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
                    return Err("samples must be decimal digits".into());
                }
                let value = value.parse::<usize>().map_err(|_| "samples out of range")?;
                if !(1..=MAX_SAMPLES).contains(&value) {
                    return Err("samples must be in 1..101".into());
                }
                samples = Some(value);
            }
            _ => return Err("unknown or repeated option".into()),
        }
    }
    Ok(Config {
        source,
        output: output.ok_or("--output-dir is required")?,
        root,
        samples: samples.unwrap_or(7),
    })
}

fn quote(value: &str) -> String {
    let mut output = String::from("\"");
    for character in value.chars() {
        match character {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            c if c <= '\u{1f}' => write!(output, "\\u{:04x}", c as u32).unwrap(),
            c => output.push(c),
        }
    }
    output.push('"');
    output
}

struct Sample {
    kind: &'static str,
    index: usize,
    nanoseconds: u128,
    verified: bool,
}
struct Report {
    status: &'static str,
    outcome: &'static str,
    active: &'static str,
    requested: usize,
    sources: Vec<(String, usize)>,
    functions: usize,
    records: usize,
    unions: usize,
    samples: Vec<Sample>,
    failure: Option<String>,
}
impl Report {
    fn new(requested: usize) -> Self {
        Self {
            status: "running",
            outcome: "pending",
            active: "load",
            requested,
            sources: Vec::new(),
            functions: 0,
            records: 0,
            unions: 0,
            samples: Vec::with_capacity(requested + 1),
            failure: None,
        }
    }

    fn save(&self, output: &Path) -> io::Result<()> {
        let mut text = format!(
            "{{\"schema_version\":1,\"tool\":\"verify-ir-profile\",\"measurement\":\"repeated-warm-checked-ir-verification\",\
             \"status\":{},\"outcome\":{},\"active_phase\":{},\"requested_samples\":{},\"warmup_calls\":1,\
             \"maximum_samples\":{MAX_SAMPLES},\"deadline_seconds\":{},\
             \"scope\":{{\"counting_allocator\":false,\"loads_and_checks_once\":true,\
             \"check_includes_verification\":true,\"explicit_samples_repeat_check_verification\":true,\
             \"source_executed\":false,\"end_to_end_compile_timing\":false,\
             \"timing\":\"Instant elapsed nanoseconds including verifier call and black_box result observation; no report IO in the timed interval\",\
             \"cache\":\"one already checked immutable IR; all explicit calls are warm-IR repetitions; reporting occurs between samples\",\
             \"limits\":\"compiler input/IR budgets plus cooperative 30-second total deadline; an external process timeout remains required\",\
             \"external_evidence\":\"caller must record source/tool/input hashes, host, configuration, CPU/RSS and distributions; these raw durations make no performance comparison\"}},\
             \"ir_counts\":{{\"functions\":{},\"records\":{},\"unions\":{}}},\"sources\":[",
            quote(self.status),
            quote(self.outcome),
            if self.active.is_empty() { "null".into() } else { quote(self.active) },
            self.requested,
            DEADLINE.as_secs(),
            self.functions,
            self.records,
            self.unions,
        );
        for (index, (name, bytes)) in self.sources.iter().enumerate() {
            if index != 0 {
                text.push(',');
            }
            write!(
                text,
                "{{\"logical_path\":{},\"bytes\":{bytes}}}",
                quote(name)
            )
            .unwrap();
        }
        text.push_str("],\"samples\":[");
        for (position, sample) in self.samples.iter().enumerate() {
            if position != 0 {
                text.push(',');
            }
            write!(
                text,
                "{{\"kind\":{},\"index\":{},\"nanoseconds\":{},\"verified\":{}}}",
                quote(sample.kind),
                sample.index,
                sample.nanoseconds,
                sample.verified
            )
            .unwrap();
        }
        writeln!(
            text,
            "],\"observed_successful_explicit_calls\":{},\"failure\":{}}}",
            self.samples.iter().filter(|sample| sample.verified).count(),
            self.failure
                .as_deref()
                .map(quote)
                .unwrap_or_else(|| "null".into()),
        )
        .unwrap();
        if text.len() > 1024 * 1024 {
            return Err(io::Error::other("verification report exceeds 1 MiB"));
        }
        let temporary = output.join("verification.json.tmp");
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)?;
        file.write_all(text.as_bytes())?;
        file.sync_all()?;
        drop(file);
        fs::rename(temporary, output.join("verification.json"))
    }

    fn reject(&mut self, output: &Path, outcome: &'static str, error: String) -> io::Result<i32> {
        self.status = "failed";
        self.outcome = outcome;
        self.active = "";
        self.failure = Some(error);
        self.save(output)?;
        Ok(1)
    }
}

fn execute(config: Config) -> io::Result<i32> {
    // This run owns an exclusively new directory; partial output is never cleaned.
    fs::create_dir(&config.output)?;
    let started = Instant::now();
    let mut report = Report::new(config.samples);
    report.save(&config.output)?;
    let project = match Project::load(&config.source, config.root.as_deref()) {
        Ok(project) => project,
        Err(error) => {
            let message = match error {
                ProjectError::Driver(message) => message,
                ProjectError::Diagnostics { diagnostics, .. } => format!("{diagnostics:#?}"),
            };
            return report.reject(&config.output, "load_error", message);
        }
    };
    report.sources = project
        .sources
        .files()
        .iter()
        .map(|source| (source.logical_path.clone(), source.text.len()))
        .collect();
    report.active = "check";
    report.save(&config.output)?;
    let program = match project.check() {
        Ok(program) => program,
        Err(errors) => return report.reject(&config.output, "check_error", format!("{errors:#?}")),
    };
    report.functions = program.functions.len();
    report.records = program.records.len();
    report.unions = program.unions.len();
    report.active = "verify-ir";
    report.save(&config.output)?;
    for call in 0..=config.samples {
        if started.elapsed() >= DEADLINE {
            return report.reject(
                &config.output,
                "deadline",
                "30-second total deadline exceeded".into(),
            );
        }
        let begin = Instant::now();
        let result = axeon::verify::verify(std::hint::black_box(&program));
        let _ = std::hint::black_box(&result);
        let nanoseconds = begin.elapsed().as_nanos();
        report.samples.push(Sample {
            kind: if call == 0 { "warmup" } else { "measured" },
            index: if call == 0 { 0 } else { call - 1 },
            nanoseconds,
            verified: result.is_ok(),
        });
        if let Err(errors) = result {
            return report.reject(&config.output, "verification_error", format!("{errors:#?}"));
        }
        report.save(&config.output)?;
    }
    if started.elapsed() >= DEADLINE {
        return report.reject(
            &config.output,
            "deadline",
            "30-second total deadline exceeded".into(),
        );
    }
    report.status = "complete";
    report.outcome = "success";
    report.active = "";
    report.save(&config.output)?;
    Ok(0)
}

fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() == 1 && args[0] == "--help" {
        print!("{HELP}");
        return;
    }
    let exit = match config(args) {
        Ok(config) => match execute(config) {
            Ok(exit) => exit,
            Err(error) => {
                eprintln!("verification profiler: {error}");
                2
            }
        },
        Err(error) => {
            eprintln!("verification profiler: {error}");
            2
        }
    };
    std::process::exit(exit);
}
