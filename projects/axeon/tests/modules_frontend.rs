//! Independent project-loader and namespace acceptance/rejection fixtures.
use axeon::diagnostic::{Diagnostic, SourceId, SourceTable, Span};
use axeon::project::{Project, ProjectError};
use std::fmt::Write;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT: AtomicUsize = AtomicUsize::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "axeon-modules-frontend-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
    fn write(&self, name: &str, text: &str) {
        fs::write(self.0.join(name), text).unwrap();
    }
    fn load(&self) -> Result<Project, ProjectError> {
        Project::load(&self.0.join("main.ax"), None)
    }
    fn rejects(&self, code: &str) -> (SourceTable, Vec<Diagnostic>) {
        match self.load().unwrap_err() {
            ProjectError::Diagnostics {
                sources,
                diagnostics,
                module_mode,
            } => {
                assert!(module_mode);
                assert!(
                    diagnostics.iter().any(|error| error.code == code),
                    "{diagnostics:?}"
                );
                assert!(diagnostics.len() <= axeon::project::MAX_PROJECT_DIAGNOSTICS);
                (sources, diagnostics)
            }
            error => panic!("expected language diagnostics, got {error:?}"),
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn result(project: &Project) -> i64 {
    axeon::interpreter::run(&project.check().unwrap(), 100_000)
        .result
        .unwrap()
}

#[test]
fn explicit_exports_private_helpers_and_module_local_names_execute() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "import right; import left; fn local() -> i64 { return 2; } fn main() -> i64 { return left::value() + right::value() + local(); }");
    fixture.write(
        "left.ax",
        "fn local() -> i64 { return 19; } export fn value() -> i64 { return local(); }",
    );
    fixture.write("right.ax", "fn local() -> i64 { return 21; } export fn value() -> i64 { return local(); } fn main() -> f64 { return 1.0; }");
    let project = fixture.load().unwrap();
    assert!(project.is_module_project());
    assert_eq!(result(&project), 42);
    assert_eq!(project.entry, SourceId(0));
    assert_eq!(
        project
            .sources
            .files()
            .iter()
            .map(|file| file.logical_path.as_str())
            .collect::<Vec<_>>(),
        ["main.ax", "left.ax", "right.ax"]
    );
    assert_eq!(
        project
            .modules
            .iter()
            .map(|module| project
                .sources
                .get(module.source)
                .unwrap()
                .logical_path
                .as_str())
            .collect::<Vec<_>>(),
        ["left.ax", "main.ax", "right.ax"]
    );
    assert_eq!(project.modules[0].exports.len(), 1);
    assert_eq!(project.modules[0].exports[0].name, "value");
}

#[test]
fn imports_and_exports_are_contextual_and_legacy_apis_remain_usable() {
    let source =
        "fn import(export: i64) -> i64 { return export; } fn main() -> i64 { return import(42); }";
    let old = axeon::compile(source).unwrap();
    assert_eq!(axeon::interpreter::run(&old, 1000).result.unwrap(), 42);
    let fixture = Fixture::new();
    fixture.write("main.ax", source);
    let project = fixture.load().unwrap();
    assert!(!project.is_module_project());
    assert_eq!(result(&project), 42);
    let errors = axeon::compile("import helper; fn main()->i64{return 0;}").unwrap_err();
    assert_eq!(errors[0].code, "E_MODULE_CONTEXT");
}

#[test]
fn imports_do_not_reexport_names_or_expose_private_functions() {
    let fixture = Fixture::new();
    fixture.write(
        "b.ax",
        "export fn visible() -> i64 { return 1; } fn hidden() -> i64 { return 2; }",
    );
    fixture.write(
        "a.ax",
        "import b; export fn value() -> i64 { return b::visible(); }",
    );
    for call in ["a::hidden()", "b::visible()", "a::visible()", "visible()"] {
        fixture.write(
            "main.ax",
            &format!("import a; fn main()->i64{{return {call};}}"),
        );
        let errors = fixture.load().unwrap().check().unwrap_err();
        assert_eq!(errors[0].code, "T007");
        assert!(errors[0].message.contains(call.trim_end_matches("()")));
    }
    fixture.write("main.ax", "import b; fn main()->i64{return b::hidden();}");
    assert_eq!(fixture.load().unwrap().check().unwrap_err()[0].code, "T007");
}

#[test]
fn the_entry_module_alone_supplies_main() {
    let fixture = Fixture::new();
    fixture.write(
        "main.ax",
        "import helper; fn other()->i64{return helper::main();}",
    );
    fixture.write("helper.ax", "export fn main()->i64{return 42;}");
    let errors = fixture.load().unwrap().check().unwrap_err();
    assert_eq!(errors[0].code, "T002");
    assert_eq!(errors[0].span.source, SourceId(0));
}

#[test]
fn cross_file_related_locations_keep_local_offsets_and_unicode_columns() {
    let fixture = Fixture::new();
    fixture.write(
        "main.ax",
        "// 雪\nimport helper;\nfn main()->i64{return helper::identity(true);}",
    );
    fixture.write(
        "helper.ax",
        "// 雪\nexport fn identity(value:i64)->i64{return value;}",
    );
    let project = fixture.load().unwrap();
    let errors = project.check().unwrap_err();
    let error = errors.iter().find(|error| error.code == "T009").unwrap();
    assert_eq!(error.span.source, SourceId(0));
    assert_eq!(error.related[0].span.source, SourceId(1));
    let rendered = error.render_modules(&project.sources);
    assert!(rendered.starts_with("main.ax:3:"), "{rendered}");
    assert!(rendered.contains("related at helper.ax:2:"), "{rendered}");
    for location in
        std::iter::once(error.span).chain(error.related.iter().map(|related| related.span))
    {
        let source = project.sources.get(location.source).unwrap();
        assert!(location.end <= source.text.len());
        assert_eq!(
            project.sources.line_col(location),
            axeon::diagnostic::line_col(&source.text, location.start)
        );
    }
}

#[test]
fn snapshots_do_not_reread_changed_dependency_contents() {
    let fixture = Fixture::new();
    fixture.write(
        "main.ax",
        "import helper; fn main()->i64{return helper::value();}",
    );
    fixture.write("helper.ax", "export fn value()->i64{return 42;}");
    let original = fixture.load().unwrap();
    fixture.write("helper.ax", "export fn value()->i64{return 99;}");
    assert_eq!(result(&original), 42);
    assert_eq!(result(&fixture.load().unwrap()), 99);
}

#[test]
fn diamonds_are_loaded_once_and_diagnostics_are_deterministic() {
    let fixture = Fixture::new();
    fixture.write(
        "main.ax",
        "import b; import a; fn main()->i64{return a::value()+b::value();}",
    );
    fixture.write(
        "b.ax",
        "import shared; export fn value()->i64{return shared::value();}",
    );
    fixture.write("shared.ax", "export fn value()->i64{return 21;}");
    fixture.write(
        "a.ax",
        "import shared; export fn value()->i64{return shared::value();}",
    );
    let project = fixture.load().unwrap();
    assert_eq!(project.sources.files().len(), 4);
    assert_eq!(result(&project), 42);
    fixture.write("shared.ax", "export fn value()->i64{return missing;}");
    let baseline = fixture.load().unwrap().check().unwrap_err();
    for _ in 0..5 {
        assert_eq!(fixture.load().unwrap().check().unwrap_err(), baseline);
    }
}

#[test]
fn missing_duplicate_and_cyclic_imports_report_the_importer() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "import absent; fn main()->i64{return 0;}");
    let (_, errors) = fixture.rejects("E_MODULE_NOT_FOUND");
    assert_eq!(errors[0].span, Span::new(0, 14));
    fixture.write("main.ax", "import a; import a; fn main()->i64{return 0;}");
    fixture.write("a.ax", "export fn value()->i64{return 1;}");
    fixture.rejects("E_MODULE_DUPLICATE_IMPORT");
    fixture.write("main.ax", "import a; fn main()->i64{return 0;}");
    fixture.write("a.ax", "import b;");
    fixture.write("b.ax", "import a;");
    let (sources, errors) = fixture.rejects("E_MODULE_CYCLE");
    let cycle = errors
        .iter()
        .find(|error| error.code == "E_MODULE_CYCLE")
        .unwrap();
    assert_eq!(sources.get(cycle.span.source).unwrap().logical_path, "b.ax");
    assert!(cycle.message.contains("a.ax -> b.ax -> a.ax"));
}

#[test]
fn malformed_imports_paths_and_qualified_values_are_rejected() {
    for source in [
        "import ../outside; fn main()->i64{return 0;}",
        "import dir/module; fn main()->i64{return 0;}",
        "import \"helper\"; fn main()->i64{return 0;}",
        "export let state:i64=0; fn main()->i64{return 0;}",
        "fn main()->i64{return helper::value;}",
        "fn main()->i64{import helper; return 0;}",
    ] {
        assert!(axeon::parser::parse(source).is_err(), "{source}");
    }
    // Three-component calls are now the syntax for imported trait methods.
    // An unresolved path still fails checking and cannot expose runnable IR.
    let source = "fn main()->i64{return a::b::value();}";
    assert!(axeon::parser::parse(source).is_ok());
    let errors = axeon::compile(source).unwrap_err();
    assert_eq!(errors.len(), 1);
    assert_eq!(errors[0].code, "T007");
    assert!(errors[0].message.contains("a::b::value"));
}

#[test]
fn imported_parse_errors_identify_the_dependency() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "import helper; fn main()->i64{return 0;}");
    fixture.write("helper.ax", "export fn broken()->i64{return ;}");
    let (sources, errors) = fixture.rejects("E_PARSE_EXPRESSION");
    assert_eq!(
        sources.get(errors[0].span.source).unwrap().logical_path,
        "helper.ax"
    );
}

#[test]
fn aggregate_and_ownership_contracts_apply_inside_and_across_modules() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "import helper; fn main()->i64{return 0;}");
    fixture.write("helper.ax", "export fn invalid(a:&[i64])->i64{return 0;}");
    assert!(fixture
        .load()
        .unwrap()
        .check()
        .unwrap_err()
        .iter()
        .any(|error| error.code == "O003"));
    fixture.write("helper.ax", "export fn invalid()->i64 { let mut a:[i64;2]=[1,2]; let shared:&[i64]=&a; let exclusive:&mut[i64]=&mut a; return 0; }");
    assert!(fixture
        .load()
        .unwrap()
        .check()
        .unwrap_err()
        .iter()
        .any(|error| error.code == "O007"));
}

#[test]
fn regular_files_exact_case_and_hardlinks_are_enforced() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "import helper; fn main()->i64{return 0;}");
    fs::create_dir(fixture.0.join("helper.ax")).unwrap();
    fixture.rejects("E_MODULE_READ");
    fs::remove_dir(fixture.0.join("helper.ax")).unwrap();
    fixture.write("Helper.ax", "export fn value()->i64{return 0;}");
    fixture.rejects("E_MODULE_NOT_FOUND");
    fs::remove_file(fixture.0.join("Helper.ax")).unwrap();
    fs::hard_link(fixture.0.join("main.ax"), fixture.0.join("helper.ax")).unwrap();
    fixture.rejects("E_MODULE_ALIAS");
}

#[cfg(unix)]
#[test]
fn module_symlinks_are_rejected() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "import helper; fn main()->i64{return 0;}");
    fixture.write("Helper.ax", "export fn value()->i64{return 0;}");
    std::os::unix::fs::symlink("Helper.ax", fixture.0.join("helper.ax")).unwrap();
    fixture.rejects("E_MODULE_PATH");
}

#[test]
fn explicit_roots_reject_escapes() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "export fn main()->i64{return 0;}");
    let outside = Fixture::new();
    match Project::load(&fixture.0.join("main.ax"), Some(&outside.0)).unwrap_err() {
        ProjectError::Diagnostics { diagnostics, .. } => {
            assert_eq!(diagnostics[0].code, "E_MODULE_PATH")
        }
        error => panic!("{error:?}"),
    }
}

#[cfg(unix)]
#[test]
fn explicit_roots_reject_symlink_ancestors() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "export fn main()->i64{return 0;}");
    let outside = Fixture::new();
    let alias = outside.0.join("linked");
    std::os::unix::fs::symlink(&fixture.0, &alias).unwrap();
    assert!(matches!(
        Project::load(&alias.join("main.ax"), None),
        Err(ProjectError::Diagnostics { .. })
    ));
}

#[cfg(unix)]
#[test]
fn legacy_symlink_entries_still_compile_but_module_entries_do_not() {
    let fixture = Fixture::new();
    fixture.write("actual.ax", "fn main()->i64{return 42;}");
    std::os::unix::fs::symlink("actual.ax", fixture.0.join("main.ax")).unwrap();
    assert_eq!(result(&fixture.load().unwrap()), 42);
    fixture.write("actual.ax", "export fn main()->i64{return 42;}");
    fixture.rejects("E_MODULE_PATH");
}

#[test]
fn selected_root_supports_a_nested_entry_without_implicit_search_paths() {
    let fixture = Fixture::new();
    fs::create_dir(fixture.0.join("app")).unwrap();
    fixture.write(
        "app/main.ax",
        "import helper; fn main()->i64{return helper::value();}",
    );
    fixture.write("helper.ax", "export fn value()->i64{return 42;}");
    let project = Project::load(&fixture.0.join("app/main.ax"), Some(&fixture.0)).unwrap();
    assert_eq!(project.sources.files()[0].logical_path, "app/main.ax");
    assert_eq!(result(&project), 42);
    assert!(Project::load(&fixture.0.join("app/main.ax"), None).is_err());
}

#[test]
fn file_limit_counts_entry_and_admits_exactly_64_files() {
    let fixture = Fixture::new();
    let mut entry = String::new();
    for index in 0..63 {
        entry.push_str(&format!("import m{index};"));
        fixture.write(&format!("m{index}.ax"), "");
    }
    fixture.write("main.ax", &format!("{entry}fn main()->i64{{return 0;}}"));
    assert_eq!(fixture.load().unwrap().sources.files().len(), 64);
    fixture.write("m63.ax", "");
    fixture.write(
        "main.ax",
        &format!("{entry}import m63; fn main()->i64{{return 0;}}"),
    );
    fixture.rejects("E_PROJECT_FILES");
}

#[test]
fn depth_includes_entry_and_checks_the_longest_diamond_path() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "import m0; fn main()->i64{return 0;}");
    for index in 0..14 {
        fixture.write(&format!("m{index}.ax"), &format!("import m{};", index + 1));
    }
    fixture.write("m14.ax", "");
    assert!(fixture.load().is_ok());
    fixture.write("m14.ax", "import m15;");
    fixture.write("m15.ax", "");
    fixture.rejects("E_PROJECT_DEPTH");

    // Sorted traversal completes the shallow shared tail first. The second
    // route reaches that already visited tail at depth 16 and is still invalid.
    fixture.write("main.ax", "import a; import z0; fn main()->i64{return 0;}");
    fixture.write("a.ax", "import tail;");
    fixture.write("tail.ax", "");
    for index in 0..13 {
        fixture.write(&format!("z{index}.ax"), &format!("import z{};", index + 1));
    }
    fixture.write("z13.ax", "import a;");
    fixture.rejects("E_PROJECT_DEPTH");
}

#[test]
fn aggregate_edge_function_token_and_byte_limits_are_enforced() {
    let fixture = Fixture::new();
    let mut entry = String::new();
    for index in 0..32 {
        entry.push_str(&format!("import m{index};"));
        let mut imports = String::new();
        for leaf in 0..8 {
            write!(imports, "import leaf{leaf};").unwrap();
        }
        fixture.write(&format!("m{index}.ax"), &imports);
    }
    for index in 0..8 {
        fixture.write(&format!("leaf{index}.ax"), "");
    }
    fixture.write("main.ax", &format!("{entry}fn main()->i64{{return 0;}}"));
    fixture.rejects("E_PROJECT_EDGES");

    fixture.write("main.ax", "import a; import b; fn main()->i64{return 0;}");
    let mut functions = String::new();
    for index in 0..512 {
        write!(functions, "fn function_{index}()->i64{{return 0;}}").unwrap();
    }
    fixture.write("a.ax", &functions);
    fixture.write("b.ax", &functions);
    fixture.rejects("E_PROJECT_FUNCTIONS");
    fixture.write("a.ax", &";".repeat(60_000));
    fixture.write("b.ax", &";".repeat(60_000));
    fixture.rejects("E_PROJECT_TOKENS");
    let comment = format!("//{}\n", "x".repeat(524_288));
    fixture.write("a.ax", &comment);
    fixture.write("b.ax", &comment);
    fixture.rejects("E_PROJECT_BYTES");
}

#[test]
fn project_diagnostics_are_capped_and_lex_errors_have_file_identity() {
    let fixture = Fixture::new();
    fixture.write(
        "main.ax",
        "import a; import b; import c; import d; import e; fn main()->i64{return 0;}",
    );
    for name in ["a", "b", "c", "d", "e"] {
        fixture.write(&format!("{name}.ax"), &"@".repeat(100));
    }
    let (sources, errors) = fixture.rejects("E_LEX_CHAR");
    assert_eq!(errors.len(), 256);
    assert_eq!(
        sources.get(errors[0].span.source).unwrap().logical_path,
        "a.ax"
    );
    assert_eq!(
        sources.get(errors[255].span.source).unwrap().logical_path,
        "d.ax"
    );
}

#[test]
fn entry_io_errors_stay_driver_errors_and_spans_cannot_cross_sources() {
    assert!(matches!(
        Project::load(Path::new("/definitely-no-axeon-input/main.ax"), None),
        Err(ProjectError::Driver(_))
    ));
    let fixture = Fixture::new();
    fs::write(fixture.0.join("main.ax"), [0xff]).unwrap();
    assert!(
        matches!(fixture.load(), Err(ProjectError::Driver(message)) if message.contains("valid UTF-8"))
    );
    let joined =
        std::panic::catch_unwind(|| Span::new(0, 1).join(Span::in_source(SourceId(1), 0, 1)));
    assert!(joined.is_err());
}

#[test]
fn directory_spelling_index_is_bounded_and_counts_unrelated_entries() {
    use axeon::project::MAX_PROJECT_DIRECTORY_ENTRIES;
    let fixture = Fixture::new();
    fixture.write("main.ax", "export fn main()->i64{return 0;}");
    for index in 1..MAX_PROJECT_DIRECTORY_ENTRIES {
        fixture.write(&format!("unrelated-{index}"), "");
    }
    assert_eq!(result(&fixture.load().unwrap()), 0);
    fixture.write("one-too-many", "");
    let (_, first) = fixture.rejects("E_PROJECT_DIRECTORY");
    let (_, second) = fixture.rejects("E_PROJECT_DIRECTORY");
    assert_eq!(first, second);
    assert_eq!(first[0].span, Span::default());
}

#[cfg(unix)]
#[test]
fn literal_backslashes_in_unix_filenames_do_not_alias_nested_logical_paths() {
    let fixture = Fixture::new();
    fs::create_dir(fixture.0.join("a")).unwrap();
    let text = "export fn main()->i64{return missing;}";
    fixture.write(r"a\b.ax", text);
    fixture.write("a/b.ax", text);
    let literal = Project::load(&fixture.0.join(r"a\b.ax"), Some(&fixture.0)).unwrap();
    let nested = Project::load(&fixture.0.join("a/b.ax"), Some(&fixture.0)).unwrap();
    assert_eq!(literal.sources.files()[0].logical_path, r"a\b.ax");
    assert_eq!(nested.sources.files()[0].logical_path, "a/b.ax");
    assert_ne!(
        literal.modules[0].canonical_path,
        nested.modules[0].canonical_path
    );
    assert!(literal.check().unwrap_err()[0]
        .render_modules(&literal.sources)
        .starts_with(r"a\b.ax:"));
    assert!(nested.check().unwrap_err()[0]
        .render_modules(&nested.sources)
        .starts_with("a/b.ax:"));
}

#[cfg(windows)]
#[test]
fn windows_path_separators_share_one_normalized_logical_source() {
    let fixture = Fixture::new();
    fs::create_dir(fixture.0.join("a")).unwrap();
    let text = "export fn main()->i64{return missing;}";
    fixture.write("a/b.ax", text);
    let backslash = Project::load(&fixture.0.join(r"a\b.ax"), Some(&fixture.0)).unwrap();
    let slash = Project::load(&fixture.0.join("a/b.ax"), Some(&fixture.0)).unwrap();
    assert_eq!(backslash.sources.files()[0].logical_path, "a/b.ax");
    assert_eq!(slash.sources.files()[0].logical_path, "a/b.ax");
    assert_eq!(
        backslash.modules[0].canonical_path,
        slash.modules[0].canonical_path
    );
    let first = backslash.check().unwrap_err()[0].render_modules(&backslash.sources);
    let second = slash.check().unwrap_err()[0].render_modules(&slash.sources);
    assert_eq!(first, second);
    assert!(first.starts_with("a/b.ax:"));
}
