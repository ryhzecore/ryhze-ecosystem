use axeon::{
    ast::{self, Type},
    diagnostic::{Diagnostic, SourceId, SourceTable},
    interpreter, llvm,
    project::{ExportField, GenericExport, NominalKind, Project, ProjectError, TypeDefinition},
};
use std::{
    collections::BTreeMap,
    env, fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};

mod digest;
mod native_host;
mod nominal_metadata;
mod workflow;

const CLANG_VERSION: &str = "19.1.7";
const HELP: &str = "Axeon CPU compiler foundation 0.13.0\n\
Usage: axeon <check|interpret|emit-ir|emit-object|build|run|test> SOURCE [options]\n\
  axeon new DIRECTORY     Create a new local project\n\
  axeon format SOURCE [--check]  Canonical source to stdout, or check formatting\n\
  axeon explain CODE      Explain a diagnostic without network access\n\
  -o PATH                  Required for emit-ir, emit-object and build\n\
  --opt 0|2|3              Native optimization (default 2)\n\
  --target TRIPLE          Native host target (cross-linking is not supported)\n\
  --fuel N                 Interpreter budget (default 1000000)\n\
  --project-root PATH      Local module root (default entry directory)\n\
  --diagnostic-format text|json\n\
  --version | --help\n\
Native commands require Clang 19.1.7; set AXEON_CLANG to its executable.\n\
Tensor linking needs the platform runtime archive beside this compiler or AXEON_TENSOR_RUNTIME.\n\
The reference interpreter is exposed as interpret. Native run has a 5s limit.";

fn main() {
    std::process::exit(match execute() {
        Ok(code) => code,
        Err(msg) => {
            eprintln!("error[D001]: {msg}");
            2
        }
    });
}

fn execute() -> Result<i32, String> {
    let mut args = env::args().skip(1);
    let Some(command) = args.next() else {
        println!("{HELP}");
        return Ok(0);
    };
    if command == "--help" {
        if args.next().is_some() {
            return Err("--help does not accept source options".into());
        }
        println!("{HELP}");
        return Ok(0);
    }
    if command == "--version" {
        if args.next().is_some() {
            return Err("--version does not accept source options".into());
        }
        println!("axeon 0.13.0; subset 13; LLVM/Clang {CLANG_VERSION}; hosted ABI 1 (single-file), 2 (modules)");
        return Ok(0);
    }
    if command == "new" || command == "test" {
        return workflow::execute(&command, args.collect());
    }
    if command == "explain" {
        let code = args.next().ok_or("explain requires a diagnostic code")?;
        if args.next().is_some() {
            return Err("explain accepts exactly one diagnostic code".into());
        }
        let explanation = axeon::tooling::explain(&code)
            .ok_or_else(|| format!("unknown diagnostic code {code:?}"))?;
        println!("{code}: {explanation}");
        return Ok(0);
    }
    if command == "format" {
        return format_command(args.collect());
    }
    if ![
        "check",
        "interpret",
        "emit-ir",
        "emit-object",
        "build",
        "run",
    ]
    .contains(&command.as_str())
    {
        return Err(format!("unsupported command {command:?}; use --help"));
    }
    let path = args.next().ok_or("source path is required")?;
    let mut output = None;
    let mut project_root = None;
    let mut requested_target = None;
    let mut opt = "2".to_string();
    let mut fuel = interpreter::DEFAULT_FUEL;
    let mut json = false;
    let mut seen = std::collections::HashSet::new();
    while let Some(arg) = args.next() {
        if !seen.insert(arg.clone()) {
            return Err(format!("duplicate option {arg}"));
        }
        let value = args
            .next()
            .ok_or_else(|| format!("missing value for {arg}"))?;
        match arg.as_str() {
            "-o" => output = Some(PathBuf::from(value)),
            "--project-root" if !value.is_empty() => project_root = Some(PathBuf::from(value)),
            "--target" => requested_target = Some(value),
            "--opt" if ["0", "2", "3"].contains(&value.as_str()) => opt = value,
            "--fuel" => {
                fuel = value
                    .parse()
                    .map_err(|_| "fuel must be a nonnegative integer")?
            }
            "--diagnostic-format" if value == "json" => json = true,
            "--diagnostic-format" if value == "text" => json = false,
            _ => {
                return Err(format!(
                    "unknown option or unsupported value: {arg} {value}"
                ))
            }
        }
    }
    if ["emit-ir", "emit-object", "build"].contains(&command.as_str()) && output.is_none() {
        return Err("this command requires -o PATH".into());
    }
    if output.is_some() && !["emit-ir", "emit-object", "build"].contains(&command.as_str()) {
        return Err("-o is only valid for artifact commands".into());
    }
    if seen.contains("--fuel") && command != "interpret" {
        return Err("--fuel is only valid for interpret".into());
    }
    if seen.contains("--opt") && !["build", "run", "emit-object"].contains(&command.as_str()) {
        return Err("--opt is only valid for native commands".into());
    }
    if requested_target.is_some()
        && !["emit-ir", "emit-object", "build", "run"].contains(&command.as_str())
    {
        return Err("--target is only valid for artifact or native run commands".into());
    }
    let project = match Project::load(Path::new(&path), project_root.as_deref()) {
        Ok(project) => project,
        Err(ProjectError::Driver(message)) => return Err(message),
        Err(ProjectError::Diagnostics {
            sources,
            diagnostics,
            module_mode,
        }) => {
            if module_mode {
                print_module_diagnostics(&diagnostics, &sources, json);
            } else {
                let source = sources
                    .get(SourceId(0))
                    .map_or("", |file| file.text.as_str());
                print_diagnostics(&diagnostics, source, &path, json);
            }
            return Ok(1);
        }
    };
    let module_mode = project.is_module_project();
    let source = &project.sources.get(project.entry).unwrap().text;
    let program = match project.check() {
        Ok(p) => p,
        Err(diags) => {
            if module_mode {
                print_module_diagnostics(&diags, &project.sources, json);
            } else {
                print_diagnostics(&diags, source, &path, json);
            }
            return Ok(1);
        }
    };
    if command == "check" {
        return Ok(0);
    }
    if command == "interpret" {
        let result = interpreter::run(&program, fuel);
        print!("{}", result.stdout);
        match result.result {
            Ok(value) => {
                println!("result: {value}");
                return Ok(0);
            }
            Err(trap) => {
                std::io::stdout().flush().map_err(|e| e.to_string())?;
                eprint!(
                    "{}",
                    if module_mode {
                        trap.render_modules(&project.sources)
                    } else {
                        trap.render(source)
                    }
                );
                return Ok(70);
            }
        }
    }
    let target = native_host::target()?;
    if let Some(requested) = requested_target {
        if requested != target.triple() {
            return Err(format!("unsupported native target {requested:?}; this host supports {}; cross-linking is not qualified", target.triple()));
        }
    }
    if let Some(dest) = &output {
        protect_sources(&project, dest)?;
        if command != "emit-ir" {
            protect_sources(&project, &sidecar_path(dest))?;
        }
    }
    let ir = if module_mode {
        llvm::emit_modules_for_target(&program, &project.sources, target)
    } else {
        llvm::emit_for_target(&program, source, target)
    };
    if command == "emit-ir" {
        let dest = output.unwrap();
        protect_sources(&project, &dest)?;
        atomic_write(&dest, ir.as_bytes())?;
        return Ok(0);
    }
    let clang = native_host::clang_path()?;
    let probe = native_host::bounded(&clang, 5)?
        .arg("--version")
        .output()
        .map_err(|e| format!("cannot execute Clang (set AXEON_CLANG): {e}"))?;
    let version = String::from_utf8_lossy(&probe.stdout);
    let first = version.lines().next().unwrap_or("unknown");
    let found = first
        .split_once("clang version ")
        .and_then(|(_, tail)| tail.split_whitespace().next());
    if !probe.status.success() || found != Some(CLANG_VERSION) {
        return Err(format!(
            "native backend requires Clang {CLANG_VERSION}, found {first} (probe {}): {}",
            probe.status,
            String::from_utf8_lossy(&probe.stderr)
        ));
    }
    let temp = TempDir::new()?;
    fs::write(temp.0.join("program.ll"), &ir).map_err(|e| e.to_string())?;
    fs::write(
        temp.0.join("runtime.c"),
        include_str!("../runtime/runtime.c"),
    )
    .map_err(|e| e.to_string())?;
    fs::write(
        temp.0.join("runtime.h"),
        include_str!("../runtime/runtime.h"),
    )
    .map_err(|e| e.to_string())?;
    // Compile the exact runtime bytes whose digest is recorded below. Reading
    // once also prevents a caller replacing an external archive between the
    // digest and link steps. Object-only emission does not need an archive.
    let tensor_archive_sha = if program.uses_tensors() && command != "emit-object" {
        let bytes = read_tensor_runtime()?;
        let digest = sha256(&bytes)?;
        fs::write(temp.0.join("tensor-runtime.a"), bytes).map_err(|e| e.to_string())?;
        Some(digest)
    } else {
        None
    };
    let artifact = temp.0.join(if command == "emit-object" {
        if cfg!(windows) {
            "program.obj"
        } else {
            "program.o"
        }
    } else if cfg!(windows) {
        "program.exe"
    } else {
        "program"
    });
    let mut cmd = native_host::bounded(&clang, 30)?;
    cmd.current_dir(&temp.0)
        .env("SOURCE_DATE_EPOCH", "0")
        .args([
            "-target",
            target.triple(),
            "-Werror",
            "-Wno-override-module",
            "-ffp-contract=off",
        ])
        .arg(format!("-O{opt}"))
        .arg("program.ll");
    if cfg!(windows) {
        // COFF timestamps must not depend on wall time; this target uses lld,
        // not MSVC incremental linking. SOURCE_DATE_EPOCH alone is insufficient.
        cmd.arg("-mno-incremental-linker-compatible");
    }
    if command == "emit-object" {
        cmd.arg("-c");
    } else {
        cmd.args(["runtime.c", "-std=c11", "-fno-ident"]);
        if cfg!(windows) {
            cmd.args(["-fuse-ld=lld", "-Xlinker", "/Brepro"]);
        } else {
            cmd.arg("-Wl,--build-id=none");
        }
        if tensor_archive_sha.is_some() {
            cmd.arg("tensor-runtime.a");
            if cfg!(windows) {
                cmd.args(["-lws2_32", "-luserenv", "-lbcrypt", "-lntdll", "-ladvapi32"]);
            } else {
                cmd.args([
                    "-lgcc_s",
                    "-lutil",
                    "-lrt",
                    "-lpthread",
                    "-lm",
                    "-ldl",
                    "-lc",
                ]);
            }
        }
    }
    let status = cmd
        .arg("-o")
        .arg(&artifact)
        .output()
        .map_err(|e| format!("native compiler launch failed: {e}"))?;
    if !status.status.success() {
        return Err(format!(
            "native compilation failed ({}): {}",
            status.status,
            String::from_utf8_lossy(&status.stderr)
        ));
    }
    if command == "run" {
        return native_host::bounded(&artifact, 5)?
            .status()
            .map(|s| s.code().unwrap_or(128))
            .map_err(|e| e.to_string());
    }
    let dest = output.unwrap();
    protect_sources(&project, &dest)?;
    let artifact_bytes = fs::read(&artifact).map_err(|e| e.to_string())?;
    let source_sha = sha256(source.as_bytes())?;
    let ir_sha = sha256(ir.as_bytes())?;
    let artifact_sha = sha256(&artifact_bytes)?;
    let tensor_metadata = if program.uses_tensors() {
        format!(
            "{{\"required_abi\":1,\"required_version\":\"0.1.0\",\"native_library\":\"axeon-numerics-0.1.0\",\"basic_precision\":\"cpu-f64-canonical-storage-ordered-v1\",\"hosted_math_precision\":\"cpu-f64-exp-log1p-corpus-v1\",\"link_required\":{},\"archive_sha256\":{}}}",
            command == "emit-object",
            tensor_archive_sha.as_deref().map(quote).unwrap_or_else(|| "null".into())
        )
    } else {
        "null".into()
    };
    let kind = if command == "emit-object" {
        "object"
    } else {
        "executable"
    };
    let metadata = artifact_metadata(
        &project,
        &opt,
        kind,
        &source_sha,
        &ir_sha,
        &artifact_sha,
        &tensor_metadata,
    )?;
    let sidecar = sidecar_path(&dest);
    protect_sources(&project, &dest)?;
    protect_sources(&project, &sidecar)?;
    publish_artifact(
        &dest,
        &artifact_bytes,
        &sidecar,
        metadata.as_bytes(),
        command == "build",
    )?;
    Ok(0)
}

fn format_command(args: Vec<String>) -> Result<i32, String> {
    let (path, check) = match args.as_slice() {
        [path] => (path, false),
        [path, flag] if flag == "--check" => (path, true),
        _ => return Err("usage: axeon format SOURCE [--check]".into()),
    };
    if !fs::metadata(path)
        .map_err(|e| format!("cannot inspect source: {e}"))?
        .is_file()
    {
        return Err("format source must be a regular file".into());
    }
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::fs::OpenOptionsExt;
        // Do not wait on a FIFO substituted after the regular-file check.
        options.custom_flags(0x800);
    }
    let file = options
        .open(path)
        .map_err(|e| format!("cannot open source: {e}"))?;
    if !file.metadata().map_err(|e| e.to_string())?.is_file() {
        return Err("format source must be a regular file".into());
    }
    let mut bytes = Vec::new();
    file.take(axeon::lexer::MAX_SOURCE_BYTES as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > axeon::lexer::MAX_SOURCE_BYTES {
        return Err("format source exceeds the 1 MiB limit".into());
    }
    let source = String::from_utf8(bytes).map_err(|_| "source must be UTF-8")?;
    let formatted = match axeon::tooling::format_source(&source) {
        Ok(formatted) => formatted,
        Err(diagnostics) => {
            print_diagnostics(&diagnostics, &source, path, false);
            return Ok(1);
        }
    };
    if check {
        if source != formatted {
            eprintln!("formatting differs: {path}");
            return Ok(1);
        }
    } else {
        std::io::stdout()
            .write_all(formatted.as_bytes())
            .map_err(|e| e.to_string())?;
    }
    Ok(0)
}

fn read_tensor_runtime() -> Result<Vec<u8>, String> {
    const MAX_ARCHIVE_BYTES: u64 = 128 * 1024 * 1024;
    let path = match env::var_os("AXEON_TENSOR_RUNTIME") {
        Some(value) if !value.is_empty() => PathBuf::from(value),
        Some(_) => return Err("AXEON_TENSOR_RUNTIME must name a runtime archive".into()),
        None => env::current_exe()
            .map_err(|e| e.to_string())?
            .parent()
            .ok_or("cannot locate compiler directory")?
            .join(native_host::archive_name()),
    };
    let mut bytes = Vec::new();
    let archive = fs::File::open(&path).map_err(|e| {
        format!(
            "cannot open tensor runtime {}: {e}; build the workspace or set AXEON_TENSOR_RUNTIME",
            path.display()
        )
    })?;
    if !archive.metadata().map_err(|e| e.to_string())?.is_file() {
        return Err("tensor runtime must be a regular static archive".into());
    }
    archive
        .take(MAX_ARCHIVE_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| format!("cannot read tensor runtime: {e}"))?;
    if bytes.len() as u64 > MAX_ARCHIVE_BYTES || !bytes.starts_with(b"!<arch>\n") {
        return Err("tensor runtime must be a regular static archive of at most 128 MiB".into());
    }
    Ok(bytes)
}

fn sidecar_path(dest: &Path) -> PathBuf {
    // Append without lossy conversion of a non-UTF-8 output filename.
    let mut name = dest.as_os_str().to_owned();
    name.push(".json");
    PathBuf::from(name)
}

fn protect_sources(project: &Project, dest: &Path) -> Result<(), String> {
    let canonical_dest = match fs::canonicalize(dest) {
        Ok(path) => Some(path),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            let parent = dest
                .parent()
                .filter(|p| !p.as_os_str().is_empty())
                .unwrap_or(Path::new("."));
            match (fs::canonicalize(parent), dest.file_name()) {
                (Ok(parent), Some(name)) => Some(parent.join(name)),
                _ => None,
            }
        }
        Err(error) => return Err(format!("cannot inspect output {}: {error}", dest.display())),
    };
    let destination_metadata = match fs::metadata(dest) {
        Ok(metadata) => Some(metadata),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
        Err(error) => return Err(format!("cannot inspect output {}: {error}", dest.display())),
    };
    for module in &project.modules {
        if canonical_dest.as_ref() == Some(&module.canonical_path) {
            return Err("output cannot overwrite source".into());
        }
        if let Some(destination_metadata) = &destination_metadata {
            let source_metadata = fs::metadata(&module.canonical_path).map_err(|error| {
                format!(
                    "cannot inspect source {}: {error}",
                    module.canonical_path.display()
                )
            })?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::MetadataExt;
                if source_metadata.dev() == destination_metadata.dev()
                    && source_metadata.ino() == destination_metadata.ino()
                {
                    return Err("output cannot overwrite source".into());
                }
            }
            #[cfg(windows)]
            {
                let source = fs::File::open(&module.canonical_path).map_err(|e| e.to_string())?;
                let destination = fs::File::open(dest).map_err(|e| e.to_string())?;
                if axeon_platform::identity(&source).map_err(|e| e.to_string())?
                    == axeon_platform::identity(&destination).map_err(|e| e.to_string())?
                {
                    return Err("output cannot overwrite source".into());
                }
            }
            #[cfg(not(unix))]
            let _ = (source_metadata, destination_metadata);
        }
    }
    Ok(())
}

#[derive(Clone)]
struct ModuleEvidence {
    path: String,
    content: String,
    interface_schema: u32,
    interface: String,
    dependency: String,
    exports: String,
    type_exports: String,
    trait_fields: String,
    generic_fields: String,
    imports: String,
}

impl ModuleEvidence {
    fn json(&self) -> String {
        format!(
            "{{\"path\":{},\"content_sha256\":{},\"interface_schema\":{},\"interface_sha256\":{},\"dependency_sha256\":{},\"exports\":{},\"type_exports\":{},\"imports\":{}{}{}}}",
            quote(&self.path), quote(&self.content), self.interface_schema, quote(&self.interface),
            quote(&self.dependency), self.exports, self.type_exports, self.imports, self.trait_fields, self.generic_fields
        )
    }
}

fn scalar_interface(ty: Type) -> bool {
    matches!(ty, Type::I64 | Type::F64 | Type::Bool)
}

fn interface_fields(
    project: &Project,
    fields: &[ExportField],
    definitions: &mut Vec<TypeEvidence>,
    visiting: &mut Vec<Type>,
) -> Result<String, String> {
    let mut records = Vec::with_capacity(fields.len());
    for field in fields {
        if matches!(field.ty, Type::Record(_) | Type::Union(_)) {
            nominal_definition(project, field.ty, definitions, visiting)?;
        }
        records.push(format!(
            "{{\"name\":{},\"type\":{}}}",
            quote(&field.name),
            interface_type(project, field.ty, definitions)?
        ));
    }
    Ok(format!("[{}]", records.join(",")))
}

fn nominal_definition(
    project: &Project,
    ty: Type,
    definitions: &mut Vec<TypeEvidence>,
    visiting: &mut Vec<Type>,
) -> Result<(), String> {
    if definitions.iter().any(|definition| definition.ty == ty) {
        return Ok(());
    }
    // Checked layouts form a finite DAG. Keep a separate defensive cycle and
    // declaration bound here, so metadata cannot recurse forever if its caller
    // ever violates that precondition. Shared children are hashed only once.
    if visiting.contains(&ty) || visiting.len() >= 256 {
        return Err(
            "exported type definition is recursive or exceeds the declaration bound".into(),
        );
    }
    visiting.push(ty);
    let identity = project
        .nominal_identity(ty)
        .ok_or("exported nominal type has no frozen logical identity")?;
    let exported = project
        .modules
        .iter()
        .flat_map(|module| &module.type_exports)
        .find(|exported| exported.ty == ty)
        .ok_or("exported interface uses a nominal type without an exported definition")?;
    let kind = match identity.kind {
        NominalKind::Record => "record",
        NominalKind::Union => "union",
    };
    let shape = match &exported.definition {
        TypeDefinition::Record { fields } => {
            format!(
                "\"fields\":{}",
                interface_fields(project, fields, definitions, visiting)?
            )
        }
        TypeDefinition::Union { variants } => {
            let mut records = Vec::with_capacity(variants.len());
            for variant in variants {
                records.push(format!(
                    "{{\"name\":{},\"fields\":{}}}",
                    quote(&variant.name),
                    interface_fields(project, &variant.fields, definitions, visiting)?
                ));
            }
            format!("\"variants\":[{}]", records.join(","))
        }
    };
    // Logical module/name/kind and declaration-order shape constitute the
    // public identity. SourceId, byte offsets and physical paths never enter it.
    let definition = format!(
        "{{\"kind\":{},\"module\":{},\"name\":{},{shape}}}",
        quote(kind),
        quote(identity.logical_path),
        quote(identity.name)
    );
    let digest = sha256(format!("axeon-export-type-v2\n{definition}").as_bytes())?;
    visiting.pop();
    definitions.push(TypeEvidence {
        ty,
        definition,
        digest,
    });
    Ok(())
}

struct TypeEvidence {
    ty: Type,
    definition: String,
    digest: String,
}

fn type_evidence(ty: Type, definitions: &[TypeEvidence]) -> Result<&TypeEvidence, String> {
    definitions
        .iter()
        .find(|definition| definition.ty == ty)
        .ok_or_else(|| "exported nominal type is missing its canonical definition".into())
}

fn interface_type(
    project: &Project,
    ty: Type,
    definitions: &[TypeEvidence],
) -> Result<String, String> {
    match ty {
        Type::I64 | Type::F64 | Type::Bool | Type::Unit => Ok(quote(&ty.to_string())),
        Type::Array { element, length } => Ok(format!(
            "{{\"kind\":\"array\",\"element\":{},\"length\":{length}}}",
            quote(&element.to_string())
        )),
        Type::Record(_) | Type::Union(_) => {
            let identity = project
                .nominal_identity(ty)
                .ok_or("exported nominal type has no frozen logical identity")?;
            let kind = match identity.kind {
                NominalKind::Record => "record",
                NominalKind::Union => "union",
            };
            let digest = &type_evidence(ty, definitions)?.digest;
            Ok(format!(
                "{{\"kind\":{},\"module\":{},\"name\":{},\"definition_sha256\":{}}}",
                quote(kind),
                quote(identity.logical_path),
                quote(identity.name),
                quote(digest)
            ))
        }
        _ => Err("unsupported exported interface type".into()),
    }
}

struct TraitEvidence {
    id: ast::RecordId,
    definition: String,
    digest: String,
}
fn trait_evidence(
    project: &Project,
    definitions: &[TypeEvidence],
) -> Result<Vec<TraitEvidence>, String> {
    let mut result = Vec::new();
    for module in &project.modules {
        let file = project
            .sources
            .get(module.source)
            .ok_or("trait source is unavailable")?;
        for contract in &module.trait_exports {
            if nominal_metadata::trait_needs_v5(project, contract.id)? {
                continue;
            }
            let descriptor = |ty| match ty {
                ast::TraitType::SelfType => Ok("{\"kind\":\"self\"}".to_owned()),
                ast::TraitType::Concrete(ty) => interface_type(project, ty, definitions),
            };
            let mut methods = Vec::new();
            for method in &contract.methods {
                let params = method
                    .params
                    .iter()
                    .map(|param| descriptor(param.ty))
                    .collect::<Result<Vec<_>, String>>()?;
                methods.push(format!(
                    "{{\"name\":{},\"params\":[{}],\"result\":{}}}",
                    quote(&method.name),
                    params.join(","),
                    descriptor(method.return_type)?
                ));
            }
            let definition = format!(
                "{{\"kind\":\"trait\",\"module\":{},\"name\":{},\"methods\":[{}]}}",
                quote(&file.logical_path),
                quote(&contract.name),
                methods.join(",")
            );
            let digest = sha256(format!("axeon-export-trait-v1\n{definition}").as_bytes())?;
            result.push(TraitEvidence {
                id: contract.id,
                definition,
                digest,
            });
        }
    }
    Ok(result)
}

fn generic_definition(
    export: &GenericExport,
    source: SourceId,
    project: &Project,
    definitions: &[TypeEvidence],
    traits: &[TraitEvidence],
) -> Result<String, String> {
    let annotation = |ty: ast::TypeExpr| match ty {
        ast::TypeExpr::Concrete(ty) => interface_type(project, ty, definitions),
        ast::TypeExpr::Parameter(id) => {
            if id.index >= export.parameters.len() || id.binder.source != source {
                return Err("generic signature has an invalid parameter identity".into());
            }
            // Only the declaration-relative ordinal is durable, never its source
            // table index or byte offset in a particular checkout.
            Ok(format!("{{\"parameter\":{}}}", id.index))
        }
        ast::TypeExpr::Application(_) => {
            Err("generic nominal application requires schema 5 metadata".into())
        }
    };
    let mut parameters = Vec::new();
    for parameter in &export.parameters {
        let mut bounds = Vec::new();
        for bound in &parameter.bounds {
            let contract = project
                .trait_definition(source, &bound.name)
                .ok_or("generic bound trait is unavailable")?;
            let file = project
                .sources
                .get(contract.id.source)
                .ok_or("generic bound source is unavailable")?;
            let evidence = traits
                .iter()
                .find(|evidence| evidence.id == contract.id)
                .ok_or("generic export bound has no public canonical definition")?;
            bounds.push(format!(
                "{{\"module\":{},\"name\":{},\"definition_sha256\":{}}}",
                quote(&file.logical_path),
                quote(&contract.name),
                quote(&evidence.digest)
            ));
        }
        bounds.sort();
        parameters.push(format!(
            "{{\"name\":{},\"bounds\":[{}]}}",
            quote(&parameter.name),
            bounds.join(",")
        ));
    }
    let params = export
        .params
        .iter()
        .map(|ty| annotation(*ty))
        .collect::<Result<Vec<_>, _>>()?;
    let file = project
        .sources
        .get(source)
        .ok_or("generic definition source is unavailable")?;
    if export.body_span.source != source {
        return Err("generic definition body belongs to a different source".into());
    }
    let body = file
        .text
        .get(export.body_span.start..export.body_span.end)
        .ok_or("generic definition body span is invalid")?;
    let body_sha = sha256(format!("axeon-generic-body-v1\n{body}").as_bytes())?;
    Ok(format!(
        "{{\"module\":{},\"name\":{},\"parameters\":[{}],\"params\":[{}],\"result\":{},\"body_sha256\":{}}}",
        quote(&file.logical_path),
        quote(&export.name),
        parameters.join(","),
        params.join(","),
        annotation(export.return_type)?,
        quote(&body_sha)
    ))
}

fn module_evidence(
    source: SourceId,
    project: &Project,
    definitions: &[TypeEvidence],
    traits: &[TraitEvidence],
    memo: &mut BTreeMap<SourceId, ModuleEvidence>,
) -> Result<ModuleEvidence, String> {
    if let Some(evidence) = memo.get(&source) {
        return Ok(evidence.clone());
    }
    if let Some(evidence) =
        nominal_metadata::module_evidence(source, project, definitions, traits, memo)?
    {
        memo.insert(source, evidence.clone());
        return Ok(evidence);
    }
    // The loader already rejects cycles and bounds depth and graph size. Module
    // order is logical-path order, which is intentionally not SourceId order.
    let module = project
        .modules
        .iter()
        .find(|module| module.source == source)
        .ok_or("module evidence refers to an unknown source")?;
    let file = project
        .sources
        .get(source)
        .ok_or("module source is unavailable")?;
    let mut exports: Vec<_> = module.exports.iter().collect();
    exports.sort_by(|a, b| a.name.cmp(&b.name));
    let mut trait_exports: Vec<_> = module.trait_exports.iter().collect();
    trait_exports.sort_by(|left, right| left.name.cmp(&right.name));
    let mut trait_definitions = Vec::new();
    let mut trait_records = Vec::new();
    for contract in trait_exports {
        let evidence = traits
            .iter()
            .find(|e| e.id == contract.id)
            .ok_or("exported trait has no canonical definition")?;
        trait_definitions.push(evidence.definition.clone());
        trait_records.push(format!(
            "{{\"name\":{},\"definition\":{},\"definition_sha256\":{}}}",
            quote(&contract.name),
            evidence.definition,
            quote(&evidence.digest)
        ));
    }
    let mut implementation_definitions = Vec::new();
    for implementation in &module.implementations {
        let contract = project
            .trait_definition(source, &implementation.trait_name)
            .ok_or("implementation trait is unavailable")?;
        if !contract.exported || !project.type_is_public(implementation.target) {
            continue;
        }
        let trait_file = project
            .sources
            .get(contract.id.source)
            .ok_or("trait source is unavailable")?;
        let evidence = traits
            .iter()
            .find(|e| e.id == contract.id)
            .ok_or("implementation trait has no canonical definition")?;
        // Signature equality was checked; the trait plus concrete target describes
        // it completely. Bound function names are local, never process addresses.
        let mut methods: Vec<_> = implementation.bindings.iter().collect();
        methods.sort_by(|left, right| left.method.cmp(&right.method));
        let methods = methods
            .iter()
            .map(|binding| {
                format!(
                    "{{\"method\":{},\"function\":{}}}",
                    quote(&binding.method),
                    quote(&binding.function)
                )
            })
            .collect::<Vec<_>>();
        implementation_definitions.push(format!("{{\"trait\":{{\"module\":{},\"name\":{},\"definition_sha256\":{}}},\"target\":{},\"module\":{},\"methods\":[{}]}}", quote(&trait_file.logical_path), quote(&contract.name), quote(&evidence.digest), interface_type(project, implementation.target, definitions)?, quote(&file.logical_path), methods.join(",")));
    }
    implementation_definitions.sort();
    let mut generic_exports: Vec<_> = module.generic_exports.iter().collect();
    generic_exports.sort_by(|a, b| a.name.cmp(&b.name));
    let mut generic_definitions = Vec::new();
    let mut generic_records = Vec::new();
    for export in generic_exports {
        let definition = generic_definition(export, source, project, definitions, traits)?;
        let digest = sha256(format!("axeon-export-generic-v1\n{definition}").as_bytes())?;
        generic_records.push(format!(
            "{{\"name\":{},\"definition\":{},\"definition_sha256\":{}}}",
            quote(&export.name),
            definition,
            quote(&digest)
        ));
        generic_definitions.push(definition);
    }
    let interface_schema = if !generic_records.is_empty() {
        4
    } else if !trait_records.is_empty() || !implementation_definitions.is_empty() {
        3
    } else if module.type_exports.is_empty()
        && exports.iter().all(|export| {
            export.params.iter().all(|ty| scalar_interface(*ty))
                && scalar_interface(export.return_type)
        })
    {
        1
    } else {
        2
    };
    let mut signatures = Vec::new();
    let mut export_records = Vec::new();
    for export in exports {
        // Parameter names are not part of either call interface. Preserve the
        // scalar-only v1 recipe exactly; v2 uses canonical typed JSON.
        let (signature, digest) = if interface_schema == 1 {
            let params = export
                .params
                .iter()
                .map(ToString::to_string)
                .collect::<Vec<_>>()
                .join(",");
            let signature = format!("fn {}({params})->{}", export.name, export.return_type);
            let digest = sha256(format!("axeon-export-signature-v1\n{signature}").as_bytes())?;
            (quote(&signature), digest)
        } else {
            let params = export
                .params
                .iter()
                .map(|ty| interface_type(project, *ty, definitions))
                .collect::<Result<Vec<_>, _>>()?;
            let signature = format!(
                "{{\"name\":{},\"params\":[{}],\"result\":{}}}",
                quote(&export.name),
                params.join(","),
                interface_type(project, export.return_type, definitions)?
            );
            let digest = sha256(format!("axeon-export-signature-v2\n{signature}").as_bytes())?;
            (signature, digest)
        };
        signatures.push(signature.clone());
        export_records.push(format!(
            "{{\"name\":{},\"signature\":{},\"signature_sha256\":{}}}",
            quote(&export.name),
            signature,
            quote(&digest)
        ));
    }
    let mut type_exports: Vec<_> = module.type_exports.iter().collect();
    type_exports.sort_by(|a, b| a.name.cmp(&b.name));
    let mut public_definitions = Vec::new();
    let mut type_records = Vec::new();
    for exported in type_exports {
        let evidence = type_evidence(exported.ty, definitions)?;
        public_definitions.push(evidence.definition.clone());
        type_records.push(format!(
            "{{\"name\":{},\"definition\":{},\"definition_sha256\":{}}}",
            quote(&exported.name),
            evidence.definition,
            quote(&evidence.digest)
        ));
    }
    let content = sha256(file.text.as_bytes())?;
    let interface = if interface_schema == 1 {
        sha256(format!("axeon-module-interface-v1\n[{}]", signatures.join(",")).as_bytes())?
    } else if interface_schema == 2 {
        sha256(
            format!(
                "axeon-module-interface-v2\n{{\"module\":{},\"types\":[{}],\"functions\":[{}]}}",
                quote(&file.logical_path),
                public_definitions.join(","),
                signatures.join(",")
            )
            .as_bytes(),
        )?
    } else if interface_schema == 3 {
        sha256(format!("axeon-module-interface-v3\n{{\"module\":{},\"types\":[{}],\"functions\":[{}],\"traits\":[{}],\"implementations\":[{}]}}", quote(&file.logical_path), public_definitions.join(","), signatures.join(","), trait_definitions.join(","), implementation_definitions.join(",")).as_bytes())?
    } else {
        sha256(format!("axeon-module-interface-v4\n{{\"module\":{},\"types\":[{}],\"functions\":[{}],\"traits\":[{}],\"implementations\":[{}],\"generics\":[{}]}}", quote(&file.logical_path), public_definitions.join(","), signatures.join(","), trait_definitions.join(","), implementation_definitions.join(","), generic_definitions.join(",")).as_bytes())?
    };
    let mut imports: Vec<_> = module.imports.iter().collect();
    imports.sort_by(|a, b| a.name.cmp(&b.name));
    let mut import_records = Vec::new();
    for import in imports {
        let dependency = module_evidence(import.source, project, definitions, traits, memo)?;
        import_records.push(format!(
            "{{\"name\":{},\"path\":{},\"interface_sha256\":{},\"dependency_sha256\":{}}}",
            quote(&import.name),
            quote(&dependency.path),
            quote(&dependency.interface),
            quote(&dependency.dependency)
        ));
    }
    let imports = format!("[{}]", import_records.join(","));
    let dependency_inputs = format!(
        "{{\"path\":{},\"content_sha256\":{},\"interface_sha256\":{},\"imports\":{imports}}}",
        quote(&file.logical_path),
        quote(&content),
        quote(&interface)
    );
    let dependency = sha256(format!("axeon-module-dependency-v1\n{dependency_inputs}").as_bytes())?;
    let evidence = ModuleEvidence {
        path: file.logical_path.clone(),
        content,
        interface_schema,
        interface,
        dependency,
        exports: format!("[{}]", export_records.join(",")),
        type_exports: format!("[{}]", type_records.join(",")),
        trait_fields: if interface_schema >= 3 {
            format!(
                ",\"trait_exports\":[{}],\"implementations\":[{}]",
                trait_records.join(","),
                implementation_definitions.join(",")
            )
        } else {
            String::new()
        },
        generic_fields: if interface_schema == 4 {
            format!(",\"generic_exports\":[{}]", generic_records.join(","))
        } else {
            String::new()
        },
        imports,
    };
    memo.insert(source, evidence.clone());
    Ok(evidence)
}

fn artifact_metadata(
    project: &Project,
    opt: &str,
    kind: &str,
    source_sha: &str,
    ir_sha: &str,
    artifact_sha: &str,
    tensor_metadata: &str,
) -> Result<String, String> {
    let target_triple = native_host::target()?.triple();
    let native_policy = native_host::native_policy();
    let module_mode = project.is_module_project();
    let schema = if module_mode { 3 } else { 2 };
    let abi = if module_mode {
        "axeon-hosted-2"
    } else {
        "axeon-hosted-1"
    };
    let module_fields = if module_mode {
        // The frozen project has at most 256 nominal declarations. Hash each
        // exported definition once, even when many signatures reference it.
        let mut definitions = Vec::new();
        let mut visiting = Vec::new();
        for exported in project
            .modules
            .iter()
            .flat_map(|module| &module.type_exports)
        {
            if nominal_metadata::type_needs_v5(project, exported.ty)? {
                continue;
            }
            nominal_definition(project, exported.ty, &mut definitions, &mut visiting)?;
        }
        let mut memo = BTreeMap::new();
        let traits = trait_evidence(project, &definitions)?;
        let entry = module_evidence(project.entry, project, &definitions, &traits, &mut memo)?;
        let mut modules: Vec<_> = memo.values().collect();
        modules.sort_by(|a, b| a.path.cmp(&b.path));
        let modules = format!(
            "[{}]",
            modules
                .iter()
                .map(|module| module.json())
                .collect::<Vec<_>>()
                .join(",")
        );
        let graph_sha = sha256(format!("axeon-module-graph-v1\n{modules}").as_bytes())?;
        let runtime_c_sha = sha256(include_bytes!("../runtime/runtime.c"))?;
        let runtime_h_sha = sha256(include_bytes!("../runtime/runtime.h"))?;
        // This compact object is an explicit, versioned hash recipe. It includes
        // the complete reachable graph and translation inputs, never physical
        // checkout/output paths or source file timestamps. It is not a cache.
        let build_inputs = format!(
            "{{\"hash_schema\":\"axeon-build-inputs-v1\",\"entry\":{},\"entry_dependency_sha256\":{},\"module_graph_sha256\":{},\"compiler\":\"axeon-0.13.0\",\"language_subset\":13,\"bootstrap_rust\":\"1.85.0\",\"llvm_clang\":\"19.1.7\",\"target\":\"{target_triple}\",\"target_features\":\"baseline-x86-64\",\"abi\":\"axeon-hosted-2\",\"runtime\":\"hosted-c-4\",\"runtime_c_sha256\":{},\"runtime_h_sha256\":{},\"precision\":\"checked-i64-strict-f64-observer-nan-v1\",\"tensor_runtime\":{tensor_metadata},\"optimization\":{opt},\"artifact_kind\":{},\"ir_sha256\":{},\"native_policy\":\"{native_policy}\",\"source_date_epoch\":0}}",
            quote(&entry.path), quote(&entry.dependency), quote(&graph_sha), quote(&runtime_c_sha), quote(&runtime_h_sha), quote(kind), quote(ir_sha)
        );
        let build_sha = sha256(format!("axeon-complete-build-v1\n{build_inputs}").as_bytes())?;
        format!(
            ",\n  \"entry\": {},\n  \"modules\": {modules},\n  \"entry_dependency_sha256\": {},\n  \"module_graph_sha256\": {},\n  \"build_inputs\": {build_inputs},\n  \"complete_build_sha256\": {}",
            quote(&entry.path), quote(&entry.dependency), quote(&graph_sha), quote(&build_sha)
        )
    } else {
        String::new()
    };
    Ok(format!(
        "{{\n  \"schema_version\": {schema},\n  \"compiler\": \"axeon-0.13.0\",\n  \"language_subset\": 13,\n  \"bootstrap_rust\": \"1.85.0\",\n  \"llvm_clang\": \"19.1.7\",\n  \"target\": \"{target_triple}\",\n  \"abi\": {},\n  \"runtime\": \"hosted-c-4\",\n  \"precision\": \"checked-i64-strict-f64-observer-nan-v1\",\n  \"tensor_runtime\": {tensor_metadata},\n  \"optimization\": {opt},\n  \"artifact_kind\": {},\n  \"source_sha256\": {},\n  \"ir_sha256\": {},\n  \"artifact_sha256\": {}{module_fields}\n}}\n",
        quote(abi), quote(kind), quote(source_sha), quote(ir_sha), quote(artifact_sha)
    ))
}

struct TempDir(PathBuf);
impl TempDir {
    fn new() -> Result<Self, String> {
        for i in 0..1000 {
            let path = env::temp_dir().join(format!("axeon-{}-{i}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Ok(Self(path)),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e.to_string()),
            }
        }
        Err("temporary-directory budget exhausted".into())
    }
}
impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn atomic_write(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let name = path
        .file_name()
        .ok_or("output must name a file")?
        .to_string_lossy();
    let tmp = path.with_file_name(format!(".{name}.{}.tmp", std::process::id()));
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&tmp)
        .map_err(|e| e.to_string())?;
    let result = file.write_all(bytes).and_then(|_| file.sync_all());
    drop(file);
    let result = result
        .and_then(|_| fs::rename(&tmp, path))
        .map_err(|e| e.to_string());
    if result.is_err() {
        let _ = fs::remove_file(tmp);
    }
    result
}
fn publish_artifact(
    path: &Path,
    bytes: &[u8],
    sidecar: &Path,
    metadata: &[u8],
    executable: bool,
) -> Result<(), String> {
    #[cfg(not(unix))]
    let _ = executable;
    // Stage both complete files before publishing either. Metadata is the commit
    // marker; consumers must verify artifact_sha256. Two-file rename cannot be
    // atomic to concurrent observers on a plain filesystem.
    for p in [path, sidecar] {
        if p.exists() && !p.is_file() {
            return Err(format!("output is not a regular file: {}", p.display()));
        }
    }
    let stage = path.with_file_name(format!(".axeon-stage-{}", std::process::id()));
    fs::create_dir(&stage).map_err(|e| e.to_string())?;
    let guard = TempDir(stage);
    let new_artifact = guard.0.join("artifact");
    let new_metadata = guard.0.join("metadata");
    fs::write(&new_artifact, bytes).map_err(|e| e.to_string())?;
    fs::write(&new_metadata, metadata).map_err(|e| e.to_string())?;
    #[cfg(unix)]
    if executable {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&new_artifact, fs::Permissions::from_mode(0o755))
            .map_err(|e| e.to_string())?;
    }
    let old_artifact = guard.0.join("old-artifact");
    let old_metadata = guard.0.join("old-metadata");
    let had_artifact = path.exists();
    let had_metadata = sidecar.exists();
    if had_artifact {
        fs::rename(path, &old_artifact).map_err(|e| e.to_string())?;
    }
    if had_metadata {
        if let Err(e) = fs::rename(sidecar, &old_metadata) {
            if had_artifact {
                let _ = fs::rename(&old_artifact, path);
            }
            return Err(e.to_string());
        }
    }
    let result = fs::rename(&new_artifact, path).and_then(|_| fs::rename(&new_metadata, sidecar));
    if let Err(e) = result {
        let _ = fs::remove_file(path);
        if had_artifact {
            let _ = fs::rename(old_artifact, path);
        }
        if had_metadata {
            let _ = fs::rename(old_metadata, sidecar);
        }
        return Err(e.to_string());
    }
    Ok(())
}
fn sha256(bytes: &[u8]) -> Result<String, String> {
    Ok(digest::sha256(bytes))
}
fn quote(value: &str) -> String {
    let mut out = "\"".to_string();
    for c in value.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c.is_control() => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}
fn print_diagnostics(diags: &[Diagnostic], source: &str, path: &str, json: bool) {
    if !json {
        for d in diags {
            eprintln!("{}", d.render(source, path));
        }
        return;
    }
    let items:Vec<_>=diags.iter().map(|d| {
        let (line,column)=axeon::diagnostic::line_col(source,d.span.start);
        let related:Vec<_>=d.related.iter().map(|r|format!("{{\"start\":{},\"end\":{},\"message\":{}}}",r.span.start,r.span.end,quote(&r.message))).collect();
        format!("{{\"code\":{},\"message\":{},\"path\":{},\"start\":{},\"end\":{},\"line\":{line},\"column\":{column},\"related\":[{}]}}",quote(d.code),quote(&d.message),quote(path),d.span.start,d.span.end,related.join(","))
    }).collect();
    eprintln!(
        "{{\"schema_version\":1,\"diagnostics\":[{}]}}",
        items.join(",")
    );
}

fn print_module_diagnostics(diags: &[Diagnostic], sources: &SourceTable, json: bool) {
    if !json {
        for diagnostic in diags {
            eprintln!("{}", diagnostic.render_modules(sources));
        }
        return;
    }
    let location = |span: axeon::diagnostic::Span| {
        let path = sources
            .get(span.source)
            .map_or("<unknown>", |file| file.logical_path.as_str());
        let (line, column) = sources.line_col(span);
        format!(
            "\"source_id\":{},\"path\":{},\"start\":{},\"end\":{},\"line\":{line},\"column\":{column}",
            span.source.0, quote(path), span.start, span.end
        )
    };
    let items: Vec<_> = diags
        .iter()
        .map(|diagnostic| {
            let related = diagnostic
                .related
                .iter()
                .map(|related| {
                    format!(
                        "{{{},\"message\":{}}}",
                        location(related.span),
                        quote(&related.message)
                    )
                })
                .collect::<Vec<_>>()
                .join(",");
            format!(
                "{{\"code\":{},\"message\":{},{},\"related\":[{related}]}}",
                quote(diagnostic.code),
                quote(&diagnostic.message),
                location(diagnostic.span)
            )
        })
        .collect();
    eprintln!(
        "{{\"schema_version\":2,\"diagnostics\":[{}]}}",
        items.join(",")
    );
}
