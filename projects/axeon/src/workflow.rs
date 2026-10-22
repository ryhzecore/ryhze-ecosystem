//! Bounded local project creation and reference-interpreter test discovery.
use axeon::{
    diagnostic::{Diagnostic, SourceTable, Span},
    interpreter,
    ir::Type,
    project::{Project, ProjectError},
};
use std::{
    collections::HashSet,
    fs::{self, OpenOptions},
    io::{self, Write},
    path::{Path, PathBuf},
};

const MAX_TESTS: usize = 64;
const MAIN_SOURCE: &str =
    "import math;\n\nfn main() -> i64 {\n    print(math::twice(21));\n    return 0;\n}\n";
const MATH_SOURCE: &str = "export fn twice(value: i64) -> i64 {\n    return value + value;\n}\n";
const TEST_SOURCE: &str = "import math;\n\n// The current compiler requires main; axeon test does not invoke it.\nfn main() -> i64 {\n    return 0;\n}\n\nfn test_twice() -> i64 {\n    if math::twice(21) == 42 {\n        return 0;\n    }\n    return 1;\n}\n\nfn test_negative() -> i64 {\n    if math::twice(-7) == -14 {\n        return 0;\n    }\n    return 1;\n}\n";

pub(super) fn execute(command: &str, args: Vec<String>) -> Result<i32, String> {
    match command {
        "new" => create_project(&args),
        "test" => run_tests(&args),
        _ => Err(format!("unsupported workflow command `{command}`")),
    }
}

fn create_project(args: &[String]) -> Result<i32, String> {
    if args.len() != 1 || args[0].is_empty() || args[0].starts_with('-') {
        return Err("usage: axeon new PATH (the directory must not exist)".to_owned());
    }
    let files = [
        ("main.ax", MAIN_SOURCE),
        ("math.ax", MATH_SOURCE),
        ("tests.ax", TEST_SOURCE),
    ]
    .into_iter()
    .map(|(name, source)| {
        axeon::tooling::format_source(source)
            .map(|source| (name, source))
            .map_err(|errors| {
                format!(
                    "internal starter formatting failed for `{name}`: {}",
                    errors
                        .iter()
                        .map(|error| error.render(source, name))
                        .collect::<Vec<_>>()
                        .join("\n")
                )
            })
    })
    .collect::<Result<Vec<_>, _>>()?;
    let path = Path::new(&args[0]);
    // create_dir is exclusive even when the existing directory is empty. Do not
    // probe then create, recurse into parents, or remove a partial result.
    fs::create_dir(path).map_err(|error| {
        format!(
            "cannot create new project directory `{}`: {error}",
            path.display()
        )
    })?;
    for (name, source) in files {
        let write = || -> io::Result<()> {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(path.join(name))?;
            file.write_all(source.as_bytes())
        };
        write().map_err(|error| {
            format!(
                "created directory `{}`, but could not write `{name}`: {error}; partial project preserved",
                path.display()
            )
        })?;
    }
    println!(
        "created project {} (main.ax, math.ax, tests.ax)",
        path.display()
    );
    Ok(0)
}

struct TestOptions<'a> {
    source: &'a str,
    root: Option<PathBuf>,
    fuel: u64,
}

fn test_options(args: &[String]) -> Result<TestOptions<'_>, String> {
    let Some(source) = args
        .first()
        .filter(|arg| !arg.is_empty() && !arg.starts_with('-'))
    else {
        return Err("usage: axeon test SOURCE [--fuel N] [--project-root PATH]".to_owned());
    };
    let mut options = TestOptions {
        source,
        root: None,
        fuel: interpreter::DEFAULT_FUEL,
    };
    let mut seen = HashSet::new();
    let mut arguments = args[1..].iter();
    while let Some(option) = arguments.next() {
        if !matches!(option.as_str(), "--fuel" | "--project-root") {
            return Err(format!("unknown test option `{option}`"));
        }
        if !seen.insert(option) {
            return Err(format!("duplicate test option `{option}`"));
        }
        let value = arguments
            .next()
            .filter(|value| !value.is_empty())
            .ok_or_else(|| format!("test option `{option}` requires a value"))?;
        if option == "--fuel" {
            if !value.bytes().all(|byte| byte.is_ascii_digit()) {
                return Err("test fuel must be a nonnegative decimal u64 integer".to_owned());
            }
            options.fuel = value
                .parse()
                .map_err(|_| "test fuel must be a nonnegative decimal u64 integer".to_owned())?;
        } else {
            options.root = Some(PathBuf::from(value));
        }
    }
    Ok(options)
}

fn diagnostics(errors: &[Diagnostic], sources: &SourceTable) {
    for error in errors {
        eprintln!("{}", error.render_modules(sources));
    }
}

fn run_tests(args: &[String]) -> Result<i32, String> {
    let options = test_options(args)?;
    let project = match Project::load(Path::new(options.source), options.root.as_deref()) {
        Ok(project) => project,
        Err(ProjectError::Driver(message)) => return Err(message),
        Err(ProjectError::Diagnostics {
            sources,
            diagnostics: errors,
            ..
        }) => {
            diagnostics(&errors, &sources);
            return Ok(1);
        }
    };
    let program = match project.check() {
        Ok(program) => program,
        Err(errors) => {
            diagnostics(&errors, &project.sources);
            return Ok(1);
        }
    };
    let mut tests: Vec<_> = program
        .functions
        .iter()
        .enumerate()
        .filter(|(_, function)| {
            function.span.source == project.entry && function.name.starts_with("test_")
        })
        .collect();
    if tests.is_empty() || tests.len() > MAX_TESTS {
        let (code, message) = if tests.is_empty() {
            (
                "E_TEST_EMPTY",
                "entry module has no test_* functions".to_owned(),
            )
        } else {
            (
                "E_TEST_LIMIT",
                format!(
                    "entry module has {} tests; limit is {MAX_TESTS}",
                    tests.len()
                ),
            )
        };
        diagnostics(
            &[Diagnostic::new(
                code,
                Span::in_source(project.entry, 0, 0),
                message,
            )],
            &project.sources,
        );
        return Ok(2);
    }
    tests.sort_by(|(_, left), (_, right)| left.name.cmp(&right.name));
    let invalid: Vec<_> = tests
        .iter()
        .filter(|(_, function)| !function.params.is_empty() || function.return_type != Type::I64)
        .map(|(_, function)| {
            Diagnostic::new(
                "E_TEST_SIGNATURE",
                function.span,
                format!(
                    "test `{}` must have no parameters and return i64",
                    function.name
                ),
            )
        })
        .collect();
    if !invalid.is_empty() {
        diagnostics(&invalid, &project.sources);
        return Ok(2);
    }

    // Keep the checked program immutable. A single cloned selection shares no
    // mutable interpreter state: run() constructs fresh state for every test.
    let mut selected = program.clone();
    let mut failed = 0;
    let mut output = io::stdout().lock();
    let write_error = |error| format!("cannot write test output: {error}");
    writeln!(
        output,
        "running {} tests (reference interpreter; fuel {} per test)",
        tests.len(),
        options.fuel
    )
    .map_err(write_error)?;
    for (id, function) in &tests {
        selected.main = *id;
        writeln!(output, "test {} ...", function.name).map_err(write_error)?;
        let execution = interpreter::run(&selected, options.fuel);
        output
            .write_all(execution.stdout.as_bytes())
            .map_err(write_error)?;
        match execution.result {
            Ok(0) => writeln!(output, "PASS {}", function.name).map_err(write_error)?,
            Ok(value) => {
                failed += 1;
                let source = project
                    .sources
                    .get(function.span.source)
                    .expect("checked source");
                let (line, column) = project.sources.line_col(function.span);
                writeln!(
                    output,
                    "FAIL {} at {}:{line}:{column}: returned {value}",
                    function.name, source.logical_path
                )
                .map_err(write_error)?;
            }
            Err(trap) => {
                failed += 1;
                writeln!(output, "FAIL {}", function.name).map_err(write_error)?;
                output.flush().map_err(write_error)?;
                eprint!(
                    "test {}: {}",
                    function.name,
                    trap.render_modules(&project.sources)
                );
            }
        }
    }
    writeln!(
        output,
        "test result: {} passed; {failed} failed",
        tests.len() - failed
    )
    .map_err(write_error)?;
    Ok(i32::from(failed != 0))
}
