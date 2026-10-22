#![deny(unsafe_code)]
#[cfg(not(all(
    target_os = "linux",
    target_pointer_width = "64",
    target_has_atomic = "64"
)))]
compile_error!("allocation profiler currently supports only 64-bit Linux with 64-bit atomics");

#[allow(unsafe_code)]
mod allocator;

use allocator::{Counting, Sample};
use axeon::diagnostic::{Diagnostic, SourceTable};
use axeon::project::{Project, ProjectError};
use std::alloc::System;
use std::ffi::OsString;
use std::fmt::Write as FormatWrite;
use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

#[global_allocator]
static ALLOCATOR: Counting<System> = Counting::new(System);

const MAX_FUEL: u64 = 10_000_000;
const MAX_PHASES: usize = 12;
const MAX_ARTIFACT_BYTES: usize = 64 * 1024 * 1024;
const HELP: &str = "axeon-allocation-profiler 0.1.0 (developer instrumentation; Linux 64-bit)\n\
usage: axeon-allocation-profiler project SOURCE --output-dir NEW_DIR [--project-root PATH] [--fuel N]\n\
       axeon-allocation-profiler format SOURCE --output-dir NEW_DIR\n\
The output directory must not exist. Fuel is 1..10000000 (default 1000000).\n\
Reports observed Rust-global allocation calls, not physical heap/RSS or timings.\n";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Project,
    Format,
}
impl Mode {
    fn name(self) -> &'static str {
        match self {
            Self::Project => "project",
            Self::Format => "format",
        }
    }
}
#[derive(Debug)]
struct Config {
    mode: Mode,
    source: PathBuf,
    directory: PathBuf,
    root: Option<PathBuf>,
    fuel: u64,
}

fn config(args: impl IntoIterator<Item = OsString>) -> Result<Config, String> {
    let mut args = args.into_iter();
    let mode = match args.next().as_deref().and_then(|s| s.to_str()) {
        Some("project") => Mode::Project,
        Some("format") => Mode::Format,
        _ => return Err(HELP.into()),
    };
    let source = PathBuf::from(args.next().ok_or("missing SOURCE")?);
    if source.as_os_str().is_empty() {
        return Err("SOURCE must not be empty".into());
    }
    let mut directory = None;
    let mut root = None;
    let mut fuel = None;
    while let Some(flag) = args.next() {
        let value = args.next().ok_or("option requires a value")?;
        if value.is_empty() {
            return Err("option value must not be empty".into());
        }
        match flag.to_str() {
            Some("--output-dir") if directory.is_none() => directory = Some(PathBuf::from(value)),
            Some("--project-root") if root.is_none() && mode == Mode::Project => {
                root = Some(PathBuf::from(value))
            }
            Some("--fuel") if fuel.is_none() && mode == Mode::Project => {
                let raw = value.to_str().ok_or("fuel must be decimal digits")?;
                if raw.is_empty() || !raw.bytes().all(|b| b.is_ascii_digit()) {
                    return Err("fuel must be decimal digits".into());
                }
                let parsed = raw.parse::<u64>().map_err(|_| "fuel is out of range")?;
                if !(1..=MAX_FUEL).contains(&parsed) {
                    return Err("fuel must be in 1..10000000".into());
                }
                fuel = Some(parsed);
            }
            _ => return Err("unknown, repeated, or inapplicable option".into()),
        }
    }
    Ok(Config {
        mode,
        source,
        directory: directory.ok_or("--output-dir is required")?,
        root,
        fuel: fuel.unwrap_or(axeon::interpreter::DEFAULT_FUEL),
    })
}

#[derive(Clone, Copy)]
struct Phase {
    name: &'static str,
    retained: &'static str,
    sample: Sample,
}
struct Artifact {
    name: &'static str,
    bytes: usize,
}
struct Source {
    name: String,
    bytes: usize,
}
struct Trap {
    code: i32,
    name: &'static str,
    source: String,
    start: usize,
    end: usize,
}
struct Report {
    mode: Mode,
    status: &'static str,
    outcome: &'static str,
    active: Option<&'static str>,
    phases: [Option<Phase>; MAX_PHASES],
    count: usize,
    skipped: Vec<&'static str>,
    sources: Vec<Source>,
    artifacts: Vec<Artifact>,
    codes: Vec<&'static str>,
    result: Option<i64>,
    trap: Option<Trap>,
    failure: Option<String>,
    fuel: u64,
}
impl Report {
    fn new(c: &Config) -> Self {
        Self {
            mode: c.mode,
            status: "running",
            outcome: "pending",
            active: None,
            phases: [None; MAX_PHASES],
            count: 0,
            skipped: Vec::new(),
            sources: Vec::new(),
            artifacts: Vec::new(),
            codes: Vec::new(),
            result: None,
            trap: None,
            failure: None,
            fuel: c.fuel,
        }
    }

    fn phase<T>(
        &mut self,
        directory: &Path,
        name: &'static str,
        retained: &'static str,
        operation: impl FnOnce() -> T,
    ) -> io::Result<T> {
        if self.count == MAX_PHASES {
            return Err(io::Error::other("phase record capacity exceeded"));
        }
        self.active = Some(name);
        self.save(directory)?;
        let start = ALLOCATOR.begin();
        let value = operation();
        // Real results are subsequently checked or written; this is not a
        // promise that Rust retains otherwise removable allocation expressions.
        std::hint::black_box(&value);
        let sample = ALLOCATOR.end(start);
        self.phases[self.count] = Some(Phase {
            name,
            retained,
            sample,
        });
        self.count += 1;
        self.active = None;
        self.save(directory)?;
        Ok(value)
    }

    fn artifact(&mut self, directory: &Path, name: &'static str, bytes: &[u8]) -> io::Result<()> {
        if bytes.len() > MAX_ARTIFACT_BYTES {
            return Err(io::Error::other("artifact exceeds 64 MiB tool limit"));
        }
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(directory.join(name))?;
        file.write_all(bytes)?;
        file.sync_all()?;
        self.artifacts.push(Artifact {
            name,
            bytes: bytes.len(),
        });
        Ok(())
    }

    fn finish(&mut self, directory: &Path, outcome: &'static str, exit: i32) -> io::Result<i32> {
        self.outcome = outcome;
        let valid = self.phases[..self.count]
            .iter()
            .flatten()
            .all(|p| p.sample.balanced());
        self.status = if valid { "complete" } else { "invalid" };
        self.save(directory)?;
        Ok(if valid { exit } else { 3 })
    }

    fn save(&self, directory: &Path) -> io::Result<()> {
        let text = self.json();
        let temporary = directory.join("report.json.tmp");
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)?;
        file.write_all(text.as_bytes())?;
        file.sync_all()?;
        drop(file);
        // This exclusively created run owns both paths. Same-directory rename
        // atomically replaces only its previous partial receipt on Linux.
        fs::rename(temporary, directory.join("report.json"))
    }

    fn json(&self) -> String {
        let mut text = String::from("{\n  \"schema_version\":1,\"profiler_version\":\"0.1.0\",\"measurement\":\"observed-rust-global-allocator-calls\",\n");
        field(&mut text, "mode", self.mode.name());
        field(&mut text, "status", self.status);
        field(&mut text, "outcome", self.outcome);
        text.push_str("\"active_phase\":");
        optional_string(&mut text, self.active);
        text.push(',');
        write!(text, "\"fuel\":{},\"max_fuel\":{MAX_FUEL},", self.fuel).unwrap();
        text.push_str("\"scope\":{");
        field(
            &mut text,
            "allocator",
            "System forwarding; observed calls in this optimized executable only",
        );
        field(
            &mut text,
            "concurrency",
            "single-threaded driver and current compiler APIs; no concurrent measured phases",
        );
        field(&mut text, "phase_lifetimes", "all-process counters remain active; entry baselines include previously retained objects and unmeasured bookkeeping");
        if self.mode == Mode::Project {
            text.push_str("\"check_includes_verification\":true,\"verify_ir_repeats_check_verification\":true,\"phases_are_disjoint_pipeline\":false,");
            field(&mut text, "explicit_verification", "verify-ir repeats boundary verification already included in check; report separately, never add phases as disjoint compilation totals");
        }
        field(&mut text, "peak", "requested live Rust allocation sizes at completed allocator-call boundaries; includes entry baseline; excludes allocator overhead and internal realloc transient storage");
        field(&mut text, "exclusions", "not physical memory, RSS, stack, source allocation expressions, libc/internal allocator memory, native program or device allocations, allocation-site attribution, or timing evidence");
        text.push_str("\"reporting_counted_in_phases\":false},");
        text.push_str("\"phases\":[");
        for (index, phase) in self.phases[..self.count].iter().flatten().enumerate() {
            if index != 0 {
                text.push(',');
            }
            phase_json(&mut text, phase);
        }
        text.push_str("],\"skipped_phases\":[");
        strings(&mut text, self.skipped.iter().copied());
        text.push_str("],\"sources\":[");
        for (index, source) in self.sources.iter().enumerate() {
            if index != 0 {
                text.push(',');
            }
            text.push('{');
            field(&mut text, "logical_path", &source.name);
            write!(text, "\"bytes\":{}}}", source.bytes).unwrap();
        }
        text.push_str("],\"artifacts\":[");
        for (index, artifact) in self.artifacts.iter().enumerate() {
            if index != 0 {
                text.push(',');
            }
            text.push('{');
            field(&mut text, "path", artifact.name);
            write!(text, "\"bytes\":{}}}", artifact.bytes).unwrap();
        }
        text.push_str("],\"diagnostic_codes\":[");
        strings(&mut text, self.codes.iter().copied());
        text.push_str("],\"result\":");
        match self.result {
            Some(value) => write!(text, "{value}").unwrap(),
            None => text.push_str("null"),
        }
        text.push_str(",\"trap\":");
        match &self.trap {
            Some(trap) => {
                text.push('{');
                field(&mut text, "name", trap.name);
                field(&mut text, "source", &trap.source);
                write!(
                    text,
                    "\"code\":{},\"start\":{},\"end\":{}}}",
                    trap.code, trap.start, trap.end
                )
                .unwrap();
            }
            None => text.push_str("null"),
        }
        text.push_str(",\"failure\":");
        optional_string(&mut text, self.failure.as_deref());
        text.push_str("\n}\n");
        text
    }
}

fn quoted(out: &mut String, value: &str) {
    out.push('"');
    for c in value.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c <= '\u{1f}' => write!(out, "\\u{:04x}", c as u32).unwrap(),
            _ => out.push(c),
        }
    }
    out.push('"');
}
fn field(out: &mut String, key: &str, value: &str) {
    quoted(out, key);
    out.push(':');
    quoted(out, value);
    out.push(',');
}
fn optional_string(out: &mut String, value: Option<&str>) {
    match value {
        Some(value) => quoted(out, value),
        None => out.push_str("null"),
    }
}
fn strings<'a>(out: &mut String, values: impl Iterator<Item = &'a str>) {
    for (index, value) in values.enumerate() {
        if index != 0 {
            out.push(',');
        }
        quoted(out, value);
    }
}
fn phase_json(out: &mut String, phase: &Phase) {
    let s = phase.sample;
    out.push('{');
    field(out, "name", phase.name);
    field(out, "retained_at_end", phase.retained);
    write!(out, "\"accounting_valid\":{},\"events\":{{", s.balanced()).unwrap();
    macro_rules! number {
        ($key:ident) => {
            write!(out, "\"{}\":{},", stringify!($key), s.events.$key).unwrap()
        };
    }
    number!(alloc_attempts);
    number!(alloc_successes);
    number!(alloc_failures);
    number!(zeroed_attempts);
    number!(zeroed_successes);
    number!(zeroed_failures);
    number!(realloc_attempts);
    number!(realloc_successes);
    number!(realloc_failures);
    number!(dealloc_calls);
    number!(allocation_requested_bytes);
    number!(allocation_successful_bytes);
    number!(realloc_requested_bytes);
    number!(realloc_successful_old_bytes);
    number!(realloc_successful_new_bytes);
    write!(
        out,
        "\"deallocated_bytes\":{}}},",
        s.events.deallocated_bytes
    )
    .unwrap();
    write!(out, "\"live_requested_bytes_begin\":{},\"live_requested_bytes_end\":{},\"live_blocks_begin\":{},\"live_blocks_end\":{},\"peak_live_requested_bytes\":{},\"peak_increase_over_begin_bytes\":{},\"net_live_requested_bytes\":{}}}",
        s.live_bytes_begin, s.live_bytes_end, s.live_blocks_begin, s.live_blocks_end,
        s.peak_live_requested_bytes, s.peak_live_requested_bytes.saturating_sub(s.live_bytes_begin),
        s.live_bytes_end as i128 - s.live_bytes_begin as i128).unwrap();
}

fn render_diagnostics(
    diagnostics: &[Diagnostic],
    sources: &SourceTable,
    modules: bool,
    path: &Path,
) -> String {
    let mut output = String::new();
    let path = path.to_string_lossy();
    for diagnostic in diagnostics {
        let rendered = if modules {
            diagnostic.render_modules(sources)
        } else {
            diagnostic.render(&sources.files()[0].text, &path)
        };
        output.push_str(&rendered);
        output.push('\n');
    }
    output
}

fn profile_project(c: &Config, report: &mut Report) -> io::Result<i32> {
    let loaded = report.phase(&c.directory, "load", "Project or load diagnostics", || {
        Project::load(&c.source, c.root.as_deref())
    })?;
    let project = match loaded {
        Ok(project) => project,
        Err(error) => {
            let message = match &error {
                ProjectError::Driver(message) => format!("{message}\n"),
                ProjectError::Diagnostics {
                    sources,
                    diagnostics,
                    module_mode,
                } => {
                    report.codes.extend(diagnostics.iter().map(|d| d.code));
                    report
                        .sources
                        .extend(sources.files().iter().map(|s| Source {
                            name: s.logical_path.clone(),
                            bytes: s.text.len(),
                        }));
                    render_diagnostics(diagnostics, sources, *module_mode, &c.source)
                }
            };
            report.artifact(&c.directory, "diagnostics.txt", message.as_bytes())?;
            drop(message);
            report.phase(
                &c.directory,
                "drop-load-error",
                "driver/report baseline",
                || drop(error),
            )?;
            report.skipped.extend([
                "check",
                "verify-ir",
                "emit-ir",
                "drop-ir-output",
                "interpret",
                "drop-execution",
                "drop-program",
                "drop-project",
            ]);
            return report.finish(&c.directory, "load_error", 1);
        }
    };
    report
        .sources
        .extend(project.sources.files().iter().map(|s| Source {
            name: s.logical_path.clone(),
            bytes: s.text.len(),
        }));
    let checked = report.phase(
        &c.directory,
        "check",
        "Project and checked IR or diagnostics; includes boundary verification",
        || project.check(),
    )?;
    let program = match checked {
        Ok(program) => program,
        Err(errors) => {
            report.codes.extend(errors.iter().map(|d| d.code));
            let message = render_diagnostics(
                &errors,
                &project.sources,
                project.is_module_project(),
                &c.source,
            );
            report.artifact(&c.directory, "diagnostics.txt", message.as_bytes())?;
            drop(message);
            report.phase(
                &c.directory,
                "drop-check-error",
                "Project and driver/report baseline",
                || drop(errors),
            )?;
            report.phase(
                &c.directory,
                "drop-project",
                "driver/report baseline",
                || drop(project),
            )?;
            report.skipped.extend([
                "verify-ir",
                "emit-ir",
                "drop-ir-output",
                "interpret",
                "drop-execution",
                "drop-program",
            ]);
            return report.finish(&c.directory, "check_error", 1);
        }
    };
    let verified = report.phase(
        &c.directory,
        "verify-ir",
        "Project, checked IR and explicit verification result; repeats check verification",
        || axeon::verify::verify(&program),
    )?;
    if let Err(errors) = verified {
        let message = format!("explicit IR verification failed: {errors:#?}\n");
        report.artifact(&c.directory, "diagnostics.txt", message.as_bytes())?;
        drop(message);
        report.phase(
            &c.directory,
            "drop-verification-error",
            "Project and checked IR",
            || drop(errors),
        )?;
        report.phase(&c.directory, "drop-program", "Project", || drop(program))?;
        report.phase(
            &c.directory,
            "drop-project",
            "driver/report baseline",
            || drop(project),
        )?;
        report
            .skipped
            .extend(["emit-ir", "drop-ir-output", "interpret", "drop-execution"]);
        return report.finish(&c.directory, "verification_error", 1);
    }
    let emitted = report.phase(
        &c.directory,
        "emit-ir",
        "Project, checked IR and emitted LLVM String",
        || {
            if project.is_module_project() {
                axeon::llvm::emit_modules(&program, &project.sources)
            } else {
                axeon::llvm::emit(&program, &project.sources.files()[project.entry.0].text)
            }
        },
    )?;
    report.artifact(&c.directory, "emitted.ll", emitted.as_bytes())?;
    report.phase(
        &c.directory,
        "drop-ir-output",
        "Project and checked IR",
        || drop(emitted),
    )?;
    let execution = report.phase(
        &c.directory,
        "interpret",
        "Project, checked IR and Execution stdout/result",
        || axeon::interpreter::run(&program, c.fuel),
    )?;
    report.artifact(&c.directory, "stdout.txt", execution.stdout.as_bytes())?;
    let (outcome, exit) = match &execution.result {
        Ok(value) => {
            report.result = Some(*value);
            ("success", 0)
        }
        Err(trap) => {
            let source = project
                .sources
                .get(trap.span.source)
                .expect("checked trap source");
            report.trap = Some(Trap {
                code: trap.kind.code(),
                name: trap.kind.name(),
                source: source.logical_path.clone(),
                start: trap.span.start,
                end: trap.span.end,
            });
            let message = if project.is_module_project() {
                trap.render_modules(&project.sources)
            } else {
                trap.render(&source.text)
            };
            report.artifact(&c.directory, "diagnostics.txt", message.as_bytes())?;
            ("trap", 1)
        }
    };
    report.phase(
        &c.directory,
        "drop-execution",
        "Project and checked IR",
        || drop(execution),
    )?;
    report.phase(&c.directory, "drop-program", "Project", || drop(program))?;
    report.phase(
        &c.directory,
        "drop-project",
        "driver/report baseline",
        || drop(project),
    )?;
    report.finish(&c.directory, outcome, exit)
}

fn read_format_source(path: &Path) -> io::Result<String> {
    if !fs::metadata(path)?.is_file() {
        return Err(io::Error::other("source must be a regular file"));
    }
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(0x800);
    }
    let file = options.open(path)?;
    if !file.metadata()?.is_file() {
        return Err(io::Error::other("source must be a regular file"));
    }
    let mut bytes = Vec::new();
    file.take(axeon::lexer::MAX_SOURCE_BYTES as u64 + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() > axeon::lexer::MAX_SOURCE_BYTES {
        return Err(io::Error::other("format source exceeds the 1 MiB limit"));
    }
    String::from_utf8(bytes).map_err(|_| io::Error::other("source must be UTF-8"))
}
fn profile_format(c: &Config, report: &mut Report) -> io::Result<i32> {
    let loaded = report.phase(
        &c.directory,
        "read-source",
        "bounded source String or IO error",
        || read_format_source(&c.source),
    )?;
    let source = match loaded {
        Ok(source) => source,
        Err(error) => {
            report.artifact(
                &c.directory,
                "diagnostics.txt",
                format!("{error}\n").as_bytes(),
            )?;
            report.phase(
                &c.directory,
                "drop-read-error",
                "driver/report baseline",
                || drop(error),
            )?;
            report
                .skipped
                .extend(["format", "drop-format-output", "drop-source"]);
            return report.finish(&c.directory, "read_error", 1);
        }
    };
    report.sources.push(Source {
        name: c
            .source
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned(),
        bytes: source.len(),
    });
    let formatted = report.phase(
        &c.directory,
        "format",
        "source and formatted String or diagnostics",
        || axeon::tooling::format_source(&source),
    )?;
    let (outcome, exit) = match &formatted {
        Ok(text) => {
            report.artifact(&c.directory, "formatted.ax", text.as_bytes())?;
            ("success", 0)
        }
        Err(diagnostics) => {
            report.codes.extend(diagnostics.iter().map(|d| d.code));
            let mut output = String::new();
            for diagnostic in diagnostics {
                output.push_str(&diagnostic.render(&source, &c.source.to_string_lossy()));
                output.push('\n');
            }
            report.artifact(&c.directory, "diagnostics.txt", output.as_bytes())?;
            ("format_error", 1)
        }
    };
    report.phase(&c.directory, "drop-format-output", "source", || {
        drop(formatted)
    })?;
    report.phase(
        &c.directory,
        "drop-source",
        "driver/report baseline",
        || drop(source),
    )?;
    report.finish(&c.directory, outcome, exit)
}

fn execute(c: Config) -> io::Result<i32> {
    // No parent creation, cleanup, overwriting or reuse of preexisting evidence.
    fs::create_dir(&c.directory)?;
    let mut report = Report::new(&c);
    report.save(&c.directory)?;
    let result = match c.mode {
        Mode::Project => profile_project(&c, &mut report),
        Mode::Format => profile_format(&c, &mut report),
    };
    if let Err(error) = &result {
        report.status = "invalid";
        report.outcome = "tool_error";
        report.failure = Some(error.to_string());
        // Preserve the previous running receipt if storage itself has failed.
        let _ = report.save(&c.directory);
    }
    result
}
fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() == 1 && args[0] == "--help" {
        print!("{HELP}");
        return;
    }
    if args.len() == 1 && args[0] == "--version" {
        println!(
            "axeon-allocation-profiler 0.1.0; observed Rust-global allocator calls; Linux 64-bit"
        );
        return;
    }
    let exit = match config(args) {
        Ok(config) => match execute(config) {
            Ok(code) => code,
            Err(error) => {
                eprintln!("allocation profiler: {error}");
                2
            }
        },
        Err(error) => {
            eprintln!("allocation profiler: {error}");
            2
        }
    };
    std::process::exit(exit);
}

#[cfg(test)]
mod tests {
    use super::*;
    fn args(items: &[&str]) -> Vec<OsString> {
        items.iter().map(OsString::from).collect()
    }
    #[test]
    fn cli_requires_explicit_unique_options_and_bounded_fuel() {
        assert_eq!(
            config(args(&["project", "x.ax", "--output-dir", "out"]))
                .unwrap()
                .fuel,
            1_000_000
        );
        for arguments in [
            vec!["project", "x.ax"],
            vec!["project", "x.ax", "--output-dir", "out", "--fuel", "0"],
            vec![
                "project",
                "x.ax",
                "--output-dir",
                "out",
                "--fuel",
                "10000001",
            ],
            vec!["project", "x.ax", "--output-dir", "out", "--fuel", "+2"],
            vec![
                "project",
                "x.ax",
                "--output-dir",
                "out",
                "--fuel",
                "1",
                "--fuel",
                "2",
            ],
            vec![
                "format",
                "x.ax",
                "--output-dir",
                "out",
                "--project-root",
                ".",
            ],
            vec!["format", "x.ax", "--output-dir", "out", "--fuel", "1"],
            vec![
                "format",
                "x.ax",
                "--output-dir",
                "out",
                "--output-dir",
                "new",
            ],
        ] {
            assert!(config(args(&arguments)).is_err());
        }
    }
    #[test]
    fn json_strings_escape_control_characters_and_preserve_unicode() {
        let mut json = String::new();
        quoted(&mut json, "a\"\\\n\r\t\0π");
        assert_eq!(json, "\"a\\\"\\\\\\n\\r\\t\\u0000π\"");
    }
}
