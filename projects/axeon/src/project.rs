//! Bounded, frozen local module graphs. Imports are compile-time declarations;
//! loading never runs source, install scripts, package managers, or initializers.
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::ffi::{OsStr, OsString};
use std::fs::{self, File, Metadata, OpenOptions};
use std::io::Read;
use std::path::{Component, Path, PathBuf};

use crate::ast;
use crate::diagnostic::{Diagnostic, SourceId, SourceTable, Span};
use crate::{check, ir, lexer, nominal, parser};

pub const MAX_PROJECT_FILES: usize = 64;
pub const MAX_PROJECT_DEPTH: usize = 16;
pub const MAX_PROJECT_EDGES: usize = 256;
pub const MAX_PROJECT_BYTES: usize = 1_048_576;
pub const MAX_PROJECT_TOKENS: usize = 100_000;
pub const MAX_PROJECT_FUNCTIONS: usize = 1024;
pub const MAX_PROJECT_GENERIC_FUNCTIONS: usize = 128;
pub const MAX_PROJECT_GENERIC_NOMINALS: usize = 128;
pub const MAX_PROJECT_NOMINALS: usize = 256;
pub const MAX_PROJECT_DIAGNOSTICS: usize = 256;
/// Counts every immediate root child, including unrelated files and directories.
pub const MAX_PROJECT_DIRECTORY_ENTRIES: usize = 4096;

#[derive(Debug)]
pub enum ProjectError {
    Driver(String),
    Diagnostics {
        sources: SourceTable,
        diagnostics: Vec<Diagnostic>,
        module_mode: bool,
    },
}

#[derive(Debug, Clone)]
pub struct Import {
    pub name: String,
    pub source: SourceId,
}
#[derive(Debug, Clone)]
pub struct Export {
    pub name: String,
    pub params: Vec<ast::Type>,
    pub return_type: ast::Type,
}
/// A frozen template interface. The body span indexes Project::sources; it is
/// not a durable identity and must not itself enter interface fingerprints.
#[derive(Debug, Clone)]
pub struct GenericExport {
    pub name: String,
    pub parameters: Vec<ast::TypeParameter>,
    pub params: Vec<ast::TypeExpr>,
    pub return_type: ast::TypeExpr,
    pub body_span: Span,
}
#[derive(Debug, Clone)]
pub struct ExportField {
    pub name: String,
    pub ty: ast::Type,
}
#[derive(Debug, Clone)]
pub struct ExportVariant {
    pub name: String,
    pub fields: Vec<ExportField>,
}
#[derive(Debug, Clone)]
pub enum TypeDefinition {
    Record { fields: Vec<ExportField> },
    Union { variants: Vec<ExportVariant> },
}
#[derive(Debug, Clone)]
pub struct TypeExport {
    pub name: String,
    pub ty: ast::Type,
    pub definition: TypeDefinition,
}
pub use crate::ast::NominalKind;
#[derive(Debug, Clone, Copy)]
pub struct NominalIdentity<'a> {
    pub kind: NominalKind,
    pub logical_path: &'a str,
    pub name: &'a str,
}
#[derive(Debug, Clone)]
pub struct Module {
    pub source: SourceId,
    pub canonical_path: PathBuf,
    pub imports: Vec<Import>,
    pub exports: Vec<Export>,
    pub generic_exports: Vec<GenericExport>,
    pub generic_nominal_exports: Vec<ast::RecordId>,
    pub type_exports: Vec<TypeExport>,
    pub trait_exports: Vec<ast::Trait>,
    pub implementations: Vec<ast::Implementation>,
}
#[derive(Debug)]
pub struct Project {
    pub sources: SourceTable,
    /// Sorted by logical path, independently of the SourceId-indexed table.
    pub modules: Vec<Module>,
    pub entry: SourceId,
    pub root: PathBuf,
    programs: Vec<ast::Program>,
    source_programs: Option<Vec<ast::SourceProgram>>,
    nominal_session: Option<nominal::NominalSession>,
    imports: Vec<Vec<(String, SourceId)>>,
    module_mode: bool,
}

impl Project {
    pub fn load(entry: &Path, root: Option<&Path>) -> Result<Self, ProjectError> {
        // Read exactly once. Legacy importless inputs retain symlink support.
        // A graph later applies stricter identity/root checks to this same read.
        let loaded = read_file(entry, false).map_err(|message| {
            ProjectError::Driver(format!("cannot read {}: {message}", entry.display()))
        })?;
        let root_input = root.unwrap_or_else(|| {
            entry
                .parent()
                .filter(|path| !path.as_os_str().is_empty())
                .unwrap_or_else(|| Path::new("."))
        });
        let root_path = fs::canonicalize(root_input)
            .map_err(|_| ProjectError::Driver("cannot resolve project root".into()))?;
        let logical = loaded
            .canonical
            .strip_prefix(&root_path)
            .ok()
            .and_then(logical_path)
            .or_else(|| {
                entry
                    .file_name()
                    .and_then(|name| name.to_str())
                    .map(str::to_owned)
            })
            .ok_or_else(|| ProjectError::Driver("entry path must be valid UTF-8".into()))?;
        let mut sources = SourceTable::new();
        sources.push(logical, loaded.text);
        let tokens =
            lexer::lex_in_source(&sources.files()[0].text, SourceId(0)).map_err(|diagnostics| {
                ProjectError::Diagnostics {
                    sources: sources.clone(),
                    diagnostics,
                    module_mode: false,
                }
            })?;
        let token_count = tokens.len() - 1;
        let syntax_mode = tokens.windows(2).any(|pair| {
            matches!(&pair[0].kind, lexer::TokenKind::Ident(name) if name == "import" || name == "export")
                && matches!(pair[1].kind, lexer::TokenKind::Fn | lexer::TokenKind::Ident(_))
        });
        let headers =
            parser::parse_headers(&tokens).map_err(|diagnostics| ProjectError::Diagnostics {
                sources: sources.clone(),
                diagnostics,
                module_mode: syntax_mode,
            })?;
        let module_mode = !headers.imports.is_empty() || headers.exported;
        if !module_mode {
            let source_program = parser::parse_source_tokens_in_scope(tokens, &HashMap::new())
                .map_err(|diagnostics| ProjectError::Diagnostics {
                    sources: sources.clone(),
                    diagnostics,
                    module_mode: false,
                })?;
            let source_programs = vec![source_program];
            let imports = vec![Vec::new()];
            let prepared = prepare_loaded(source_programs, &imports, lexer::MAX_DIAGNOSTICS)
                .map_err(|diagnostics| ProjectError::Diagnostics {
                    sources: sources.clone(),
                    diagnostics,
                    module_mode: false,
                })?;
            let implementations = prepared.programs[0].implementations.clone();
            return Ok(Self {
                sources,
                modules: vec![Module {
                    source: SourceId(0),
                    canonical_path: loaded.canonical,
                    imports: Vec::new(),
                    exports: Vec::new(),
                    generic_exports: Vec::new(),
                    generic_nominal_exports: Vec::new(),
                    type_exports: Vec::new(),
                    trait_exports: Vec::new(),
                    implementations,
                }],
                entry: SourceId(0),
                root: root_path,
                programs: prepared.programs,
                source_programs: prepared.source_programs,
                nominal_session: prepared.session,
                imports,
                module_mode: false,
            });
        }
        let error = no_symlinks(root_input)
            .and_then(|()| no_symlinks(entry))
            .and_then(|()| {
                if !root_path.is_dir() {
                    Err("project root must be a directory".to_owned())
                } else if !loaded.canonical.starts_with(&root_path) {
                    Err("entry escapes the declared project root".to_owned())
                } else {
                    // Detect a replaced entry after the initial frozen read.
                    verify_identity(entry, &loaded.identity)
                }
            });
        if let Err(message) = error {
            return Err(ProjectError::Diagnostics {
                sources,
                diagnostics: vec![Diagnostic::new("E_MODULE_PATH", Span::default(), message)],
                module_mode: true,
            });
        }
        let directory_names = match directory_index(&root_path) {
            Ok(names) => names,
            Err(diagnostic) => {
                return Err(ProjectError::Diagnostics {
                    sources,
                    diagnostics: vec![diagnostic],
                    module_mode: true,
                });
            }
        };
        let bytes = sources.files()[0].text.len();
        let functions = headers.functions;
        let generic_functions = headers.generic_functions;
        let generic_nominals = headers.generic_nominals;
        let nominals = headers.types.len();
        let mut identities = BTreeMap::new();
        identities.insert(loaded.identity, SourceId(0));
        let mut paths = BTreeMap::new();
        paths.insert(loaded.canonical.clone(), SourceId(0));
        let mut loader = Loader {
            root: root_path,
            directory_names,
            sources,
            headers: vec![Some(headers)],
            tokens_frozen: vec![Some(tokens)],
            canonical: vec![loaded.canonical],
            imports: vec![Vec::new()],
            identities,
            paths,
            stack: Vec::new(),
            finished: BTreeSet::new(),
            diagnostics: Vec::new(),
            bytes,
            tokens: token_count,
            functions,
            generic_functions,
            generic_nominals,
            nominals,
            edges: 0,
        };
        loader.visit(SourceId(0));
        if loader.diagnostics.is_empty() {
            loader.check_longest_path();
        }
        if !loader.diagnostics.is_empty() {
            return Err(ProjectError::Diagnostics {
                sources: loader.sources,
                diagnostics: loader.diagnostics,
                module_mode: true,
            });
        }
        let mut source_programs = Vec::with_capacity(loader.headers.len());
        for source in 0..loader.headers.len() {
            let mut imported = HashMap::new();
            for edge in &loader.imports[source] {
                for (name, ty, exported) in &loader.headers[edge.target.0].as_ref().unwrap().types {
                    if *exported {
                        imported.insert(format!("{}::{name}", edge.name), *ty);
                    }
                }
            }
            match parser::parse_source_tokens_in_scope(
                loader.tokens_frozen[source].take().unwrap(),
                &imported,
            ) {
                Ok(program) => source_programs.push(program),
                Err(errors) => {
                    for error in errors {
                        loader.error(error);
                    }
                }
            }
        }
        if !loader.diagnostics.is_empty() {
            return Err(ProjectError::Diagnostics {
                sources: loader.sources,
                diagnostics: loader.diagnostics,
                module_mode: true,
            });
        }
        let imports: Vec<Vec<(String, SourceId)>> = loader
            .imports
            .iter()
            .map(|edges| {
                edges
                    .iter()
                    .map(|edge| (edge.name.clone(), edge.target))
                    .collect()
            })
            .collect();
        let prepared = prepare_loaded(source_programs, &imports, MAX_PROJECT_DIAGNOSTICS).map_err(
            |diagnostics| ProjectError::Diagnostics {
                sources: loader.sources.clone(),
                diagnostics,
                module_mode: true,
            },
        )?;
        let programs = prepared.programs;
        let mut modules: Vec<_> = programs
            .iter()
            .enumerate()
            .map(|(index, program)| {
                let mut exports: Vec<_> = program
                    .functions
                    .iter()
                    .filter(|function| function.exported)
                    .map(|function| Export {
                        name: function.name.clone(),
                        params: function.params.iter().map(|param| param.ty).collect(),
                        return_type: function.return_type,
                    })
                    .collect();
                exports.sort_by(|left, right| left.name.cmp(&right.name));
                Module {
                    source: SourceId(index),
                    canonical_path: loader.canonical[index].clone(),
                    imports: loader.imports[index]
                        .iter()
                        .map(|edge| Import {
                            name: edge.name.clone(),
                            source: edge.target,
                        })
                        .collect(),
                    exports,
                    generic_exports: generic_exports(program),
                    generic_nominal_exports: generic_nominal_exports(
                        prepared
                            .source_programs
                            .as_ref()
                            .and_then(|sources| sources.get(index)),
                    ),
                    type_exports: type_exports(program),
                    trait_exports: program
                        .traits
                        .iter()
                        .filter(|contract| contract.exported)
                        .cloned()
                        .collect(),
                    implementations: program.implementations.clone(),
                }
            })
            .collect();
        modules.sort_by(|left, right| {
            loader.sources.files()[left.source.0]
                .logical_path
                .cmp(&loader.sources.files()[right.source.0].logical_path)
        });
        Ok(Self {
            sources: loader.sources,
            modules,
            entry: SourceId(0),
            root: loader.root,
            programs,
            source_programs: prepared.source_programs,
            nominal_session: prepared.session,
            imports,
            module_mode: true,
        })
    }

    pub fn is_module_project(&self) -> bool {
        self.module_mode
    }
    /// Frozen source annotations, including the arena owning their application IDs.
    pub fn source_program(&self, source: SourceId) -> Option<&ast::SourceProgram> {
        self.source_programs.as_ref()?.get(source.0)
    }
    /// Load-time normalized type metadata. Checking creates a fresh session.
    pub fn nominal_session(&self) -> Option<&nominal::NominalSession> {
        self.nominal_session.as_ref()
    }
    pub fn trait_definition(&self, source: SourceId, name: &str) -> Option<&ast::Trait> {
        check::resolve_trait(&self.programs, &self.imports, source.0, name)
    }
    pub fn type_is_public(&self, ty: ast::Type) -> bool {
        self.nominal_session.as_ref().map_or_else(
            || check::public_type(&self.programs, ty),
            |session| session.public_type(ty.into()),
        )
    }
    pub fn nominal_identity(&self, ty: ast::Type) -> Option<NominalIdentity<'_>> {
        let (id, kind) = match ty {
            ast::Type::Record(id) => (id, NominalKind::Record),
            ast::Type::Union(id) => (id, NominalKind::Union),
            _ => return None,
        };
        let declaration = id.declaration().or_else(|| {
            self.nominal_session
                .as_ref()?
                .origin(id)
                .map(|origin| origin.declaration)
        })?;
        let program = self.programs.get(declaration.source.0)?;
        let name = match kind {
            NominalKind::Record => program
                .records
                .iter()
                .find(|record| record.id == declaration.into())
                .map(|record| record.name.as_str())
                .or_else(|| {
                    self.source_program(declaration.source)?
                        .generic_records
                        .iter()
                        .find(|template| template.record.id == declaration.into())
                        .map(|template| template.record.name.as_str())
                })?,
            NominalKind::Union => program
                .unions
                .iter()
                .find(|union| union.id == declaration.into())
                .map(|union| union.name.as_str())
                .or_else(|| {
                    self.source_program(declaration.source)?
                        .generic_unions
                        .iter()
                        .find(|template| template.union.id == declaration.into())
                        .map(|template| template.union.name.as_str())
                })?,
        };
        Some(NominalIdentity {
            kind,
            logical_path: &self.sources.get(declaration.source)?.logical_path,
            name,
        })
    }
    pub fn check(&self) -> Result<ir::Program, Vec<Diagnostic>> {
        if let Some(source_programs) = &self.source_programs {
            if self.module_mode {
                check::check_modules_source(source_programs, &self.imports, self.entry)
            } else {
                check::check_source(&source_programs[0])
            }
        } else if self.module_mode {
            check::check_modules(&self.programs, &self.imports, self.entry)
        } else {
            check::check(&self.programs[0])
        }
    }
}

struct LoadedPrograms {
    programs: Vec<ast::Program>,
    source_programs: Option<Vec<ast::SourceProgram>>,
    session: Option<nominal::NominalSession>,
}

fn prepare_loaded(
    source_programs: Vec<ast::SourceProgram>,
    imports: &[Vec<(String, SourceId)>],
    diagnostic_limit: usize,
) -> Result<LoadedPrograms, Vec<Diagnostic>> {
    if source_programs.iter().any(|program| {
        !program.applications.is_empty()
            || !program.generic_records.is_empty()
            || !program.generic_unions.is_empty()
    }) {
        let prepared = nominal::prepare_sources(&source_programs, imports, diagnostic_limit)?;
        Ok(LoadedPrograms {
            programs: prepared.programs,
            source_programs: Some(source_programs),
            session: Some(prepared.session),
        })
    } else {
        let programs = source_programs
            .into_iter()
            .map(ast::SourceProgram::resolve_ordinary_annotations)
            .collect::<Result<Vec<_>, _>>()?;
        Ok(LoadedPrograms {
            programs,
            source_programs: None,
            session: None,
        })
    }
}

fn generic_nominal_exports(program: Option<&ast::SourceProgram>) -> Vec<ast::RecordId> {
    let Some(program) = program else {
        return Vec::new();
    };
    let mut exports: Vec<_> = program
        .generic_records
        .iter()
        .filter(|template| template.record.exported)
        .map(|template| (template.record.name.as_str(), template.record.id))
        .chain(
            program
                .generic_unions
                .iter()
                .filter(|template| template.union.exported)
                .map(|template| (template.union.name.as_str(), template.union.id)),
        )
        .collect();
    exports.sort_by(|left, right| left.0.cmp(right.0));
    exports
        .into_iter()
        .filter_map(|(_, id)| id.declaration())
        .collect()
}

fn generic_exports(program: &ast::Program) -> Vec<GenericExport> {
    let mut exports: Vec<_> = program
        .generic_functions
        .iter()
        .filter(|template| template.function.exported)
        .map(|template| GenericExport {
            name: template.function.name.clone(),
            parameters: template.parameters.clone(),
            params: template
                .function
                .params
                .iter()
                .map(|param| param.ty)
                .collect(),
            return_type: template.function.return_type,
            body_span: template.function.body.span,
        })
        .collect();
    exports.sort_by(|left, right| left.name.cmp(&right.name));
    exports
}

fn type_exports(program: &ast::Program) -> Vec<TypeExport> {
    fn fields(fields: &[ast::RecordField]) -> Vec<ExportField> {
        fields
            .iter()
            .map(|field| ExportField {
                name: field.name.clone(),
                ty: field.ty,
            })
            .collect()
    }
    let mut exports: Vec<_> = program
        .records
        .iter()
        .filter(|record| record.exported && record.id.declaration().is_some())
        .map(|record| TypeExport {
            name: record.name.clone(),
            ty: ast::Type::Record(record.id),
            definition: TypeDefinition::Record {
                fields: fields(&record.fields),
            },
        })
        .chain(
            program
                .unions
                .iter()
                .filter(|union| union.exported && union.id.declaration().is_some())
                .map(|union| TypeExport {
                    name: union.name.clone(),
                    ty: ast::Type::Union(union.id),
                    definition: TypeDefinition::Union {
                        variants: union
                            .variants
                            .iter()
                            .map(|variant| ExportVariant {
                                name: variant.name.clone(),
                                fields: fields(&variant.fields),
                            })
                            .collect(),
                    },
                }),
        )
        .collect();
    exports.sort_by(|left, right| left.name.cmp(&right.name));
    exports
}

#[derive(Debug, Clone)]
struct Edge {
    name: String,
    target: SourceId,
    span: Span,
}
struct Loader {
    root: PathBuf,
    directory_names: BTreeSet<OsString>,
    sources: SourceTable,
    headers: Vec<Option<parser::Headers>>,
    tokens_frozen: Vec<Option<Vec<lexer::Token>>>,
    canonical: Vec<PathBuf>,
    imports: Vec<Vec<Edge>>,
    identities: BTreeMap<Identity, SourceId>,
    paths: BTreeMap<PathBuf, SourceId>,
    stack: Vec<SourceId>,
    finished: BTreeSet<SourceId>,
    diagnostics: Vec<Diagnostic>,
    bytes: usize,
    tokens: usize,
    functions: usize,
    generic_functions: usize,
    generic_nominals: usize,
    nominals: usize,
    edges: usize,
}
impl Loader {
    fn error(&mut self, diagnostic: Diagnostic) {
        if self.diagnostics.len() < MAX_PROJECT_DIAGNOSTICS {
            self.diagnostics.push(diagnostic);
        }
    }
    fn visit(&mut self, source: SourceId) {
        if self.finished.contains(&source) {
            return;
        }
        let Some(program) = &self.headers[source.0] else {
            return;
        };
        let mut imports = program.imports.clone();
        imports.sort_by(|left, right| left.name.cmp(&right.name));
        self.stack.push(source);
        let mut previous: Option<ast::Import> = None;
        for import in imports {
            if self.diagnostics.len() == MAX_PROJECT_DIAGNOSTICS {
                break;
            }
            self.edges += 1;
            if self.edges > MAX_PROJECT_EDGES {
                self.error(Diagnostic::new(
                    "E_PROJECT_EDGES",
                    import.span,
                    format!("project exceeds {MAX_PROJECT_EDGES} import edges"),
                ));
                break;
            }
            if let Some(first) = &previous {
                if first.name == import.name {
                    self.error(
                        Diagnostic::new(
                            "E_MODULE_DUPLICATE_IMPORT",
                            import.span,
                            format!("module `{}` is already imported", import.name),
                        )
                        .with_related(first.span, "previous import"),
                    );
                    continue;
                }
            }
            previous = Some(import.clone());
            #[cfg(not(any(unix, windows)))]
            {
                self.error(Diagnostic::new(
                    "E_MODULE_HOST",
                    import.span,
                    "module loading requires a host with supported physical file identities",
                ));
                continue;
            }
            #[cfg(any(unix, windows))]
            {
                let Some(target) = self.load_import(&import) else {
                    continue;
                };
                self.imports[source.0].push(Edge {
                    name: import.name.clone(),
                    target,
                    span: import.span,
                });
                if let Some(start) = self.stack.iter().position(|&id| id == target) {
                    let witness: Vec<_> = self.stack[start..]
                        .iter()
                        .chain(std::iter::once(&target))
                        .map(|id| self.sources.files()[id.0].logical_path.as_str())
                        .collect();
                    self.error(
                        Diagnostic::new(
                            "E_MODULE_CYCLE",
                            import.span,
                            format!("module import cycle: {}", witness.join(" -> ")),
                        )
                        .with_related(
                            Span::in_source(target, 0, 0),
                            "cycle returns to this module",
                        ),
                    );
                } else if self.stack.len() >= MAX_PROJECT_DEPTH {
                    self.error(Diagnostic::new(
                        "E_PROJECT_DEPTH",
                        import.span,
                        format!(
                            "project exceeds dependency depth {MAX_PROJECT_DEPTH} including entry"
                        ),
                    ));
                } else {
                    self.visit(target);
                }
            }
        }
        self.stack.pop();
        self.finished.insert(source);
    }

    #[cfg(any(unix, windows))]
    fn load_import(&mut self, import: &ast::Import) -> Option<SourceId> {
        let file_name = format!("{}.ax", import.name);
        // Exact spelling is required even on a case-insensitive mounted volume.
        let exact = self.directory_names.contains(OsStr::new(&file_name));
        if !exact {
            self.error(Diagnostic::new(
                "E_MODULE_NOT_FOUND",
                import.span,
                format!(
                    "module `{}` requires the exact project-root file `{file_name}`",
                    import.name
                ),
            ));
            return None;
        }
        let path = self.root.join(&file_name);
        let canonical = match no_symlinks(&path).and_then(|()| {
            fs::canonicalize(&path).map_err(|_| "cannot resolve module file".to_owned())
        }) {
            Ok(path) if path.starts_with(&self.root) => path,
            Ok(_) => {
                self.error(Diagnostic::new(
                    "E_MODULE_PATH",
                    import.span,
                    "module escapes project root",
                ));
                return None;
            }
            Err(message) => {
                self.error(Diagnostic::new("E_MODULE_PATH", import.span, message));
                return None;
            }
        };
        if let Some(&source) = self.paths.get(&canonical) {
            // Repeated imports of the same frozen file implement diamonds.
            return Some(source);
        }
        if self.sources.files().len() >= MAX_PROJECT_FILES {
            self.error(Diagnostic::new(
                "E_PROJECT_FILES",
                import.span,
                format!("project exceeds {MAX_PROJECT_FILES} files including entry"),
            ));
            return None;
        }
        let loaded = match read_file(&path, true) {
            Ok(loaded) => loaded,
            Err(message) => {
                self.error(Diagnostic::new("E_MODULE_READ", import.span, message));
                return None;
            }
        };
        if loaded.canonical != canonical || !loaded.canonical.starts_with(&self.root) {
            self.error(Diagnostic::new(
                "E_MODULE_PATH",
                import.span,
                "module path changed while loading",
            ));
            return None;
        }
        if let Some(&previous) = self.identities.get(&loaded.identity) {
            self.error(
                Diagnostic::new(
                    "E_MODULE_ALIAS",
                    import.span,
                    format!(
                        "module `{}` duplicates the physical file `{}`",
                        import.name,
                        self.sources.files()[previous.0].logical_path
                    ),
                )
                .with_related(Span::in_source(previous, 0, 0), "first source identity"),
            );
            return None;
        }
        if self.bytes + loaded.text.len() > MAX_PROJECT_BYTES {
            self.error(Diagnostic::new(
                "E_PROJECT_BYTES",
                import.span,
                format!("project exceeds {MAX_PROJECT_BYTES} source bytes"),
            ));
            return None;
        }
        self.bytes += loaded.text.len();
        let id = self.sources.push(file_name, loaded.text);
        self.paths.insert(loaded.canonical.clone(), id);
        self.identities.insert(loaded.identity, id);
        self.canonical.push(loaded.canonical);
        self.headers.push(None);
        self.tokens_frozen.push(None);
        self.imports.push(Vec::new());
        let tokens = match lexer::lex_in_source(&self.sources.files()[id.0].text, id) {
            Ok(tokens) => tokens,
            Err(errors) => {
                for error in errors {
                    self.error(error);
                }
                return Some(id);
            }
        };
        self.tokens += tokens.len() - 1;
        if self.tokens > MAX_PROJECT_TOKENS {
            self.error(Diagnostic::new(
                "E_PROJECT_TOKENS",
                import.span,
                format!("project exceeds {MAX_PROJECT_TOKENS} non-EOF tokens"),
            ));
            return Some(id);
        }
        match parser::parse_headers(&tokens) {
            Ok(program) => {
                self.functions += program.functions;
                self.generic_functions += program.generic_functions;
                self.generic_nominals += program.generic_nominals;
                self.nominals += program.types.len();
                if self.functions > MAX_PROJECT_FUNCTIONS {
                    self.error(Diagnostic::new(
                        "E_PROJECT_FUNCTIONS",
                        import.span,
                        format!("project exceeds {MAX_PROJECT_FUNCTIONS} functions"),
                    ));
                } else if self.generic_functions > MAX_PROJECT_GENERIC_FUNCTIONS {
                    self.error(Diagnostic::new(
                        "E_PROJECT_GENERICS",
                        import.span,
                        format!(
                            "project exceeds {MAX_PROJECT_GENERIC_FUNCTIONS} generic declarations"
                        ),
                    ));
                } else if self.generic_nominals > MAX_PROJECT_GENERIC_NOMINALS {
                    self.error(Diagnostic::new(
                        "G002",
                        import.span,
                        format!(
                            "project exceeds {MAX_PROJECT_GENERIC_NOMINALS} generic nominal declarations"
                        ),
                    ));
                } else if self.generic_nominals > 0 && self.nominals > MAX_PROJECT_NOMINALS {
                    // Ordinary projects retain the existing load-then-check
                    // R001 boundary. Once any generic nominal is discovered,
                    // reject the entire accumulated graph before preparation.
                    self.error(Diagnostic::new(
                        "R001",
                        import.span,
                        format!(
                            "project exceeds {MAX_PROJECT_NOMINALS} record and union declarations"
                        ),
                    ));
                } else {
                    self.headers[id.0] = Some(program);
                    self.tokens_frozen[id.0] = Some(tokens);
                }
            }
            Err(errors) => {
                for error in errors {
                    self.error(error);
                }
            }
        }
        Some(id)
    }

    fn check_longest_path(&mut self) {
        // A previously visited diamond tail can make a later route deeper than
        // the initial DFS stack. Memoized longest paths enforce the actual DAG.
        fn longest(
            source: SourceId,
            edges: &[Vec<Edge>],
            memo: &mut [Option<Vec<SourceId>>],
        ) -> Vec<SourceId> {
            if let Some(path) = &memo[source.0] {
                return path.clone();
            }
            let mut tail = Vec::new();
            for edge in &edges[source.0] {
                let candidate = longest(edge.target, edges, memo);
                if candidate.len() > tail.len() {
                    tail = candidate;
                }
            }
            let mut result = vec![source];
            result.extend(tail);
            memo[source.0] = Some(result.clone());
            result
        }
        let path = longest(
            SourceId(0),
            &self.imports,
            &mut vec![None; self.imports.len()],
        );
        if path.len() > MAX_PROJECT_DEPTH {
            let edge = self.imports[path[MAX_PROJECT_DEPTH - 1].0]
                .iter()
                .find(|edge| edge.target == path[MAX_PROJECT_DEPTH])
                .unwrap();
            self.error(Diagnostic::new(
                "E_PROJECT_DEPTH",
                edge.span,
                format!("project exceeds dependency depth {MAX_PROJECT_DEPTH} including entry"),
            ));
        }
    }
}

fn logical_path(path: &Path) -> Option<String> {
    // Only actual path separators become '/'. A literal backslash in a Unix
    // filename remains part of that filename and cannot alias a nested path.
    path.components()
        .map(|component| match component {
            Component::Normal(name) => name.to_str(),
            _ => None,
        })
        .collect::<Option<Vec<_>>>()
        .map(|components| components.join("/"))
}

fn directory_index(root: &Path) -> Result<BTreeSet<OsString>, Diagnostic> {
    let entries = fs::read_dir(root).map_err(|_| {
        Diagnostic::new(
            "E_MODULE_PATH",
            Span::default(),
            "cannot inspect project root directory",
        )
    })?;
    let mut names = BTreeSet::new();
    // The first excess entry suffices to reject, regardless of filesystem order.
    for (index, entry) in entries.take(MAX_PROJECT_DIRECTORY_ENTRIES + 1).enumerate() {
        if index == MAX_PROJECT_DIRECTORY_ENTRIES {
            return Err(Diagnostic::new(
                "E_PROJECT_DIRECTORY", Span::default(),
                format!("project root exceeds {MAX_PROJECT_DIRECTORY_ENTRIES} immediate directory entries"),
            ));
        }
        let entry = entry.map_err(|_| {
            Diagnostic::new(
                "E_MODULE_PATH",
                Span::default(),
                "cannot inspect project root directory entry",
            )
        })?;
        names.insert(entry.file_name());
    }
    Ok(names)
}

/// Check every path component before following it, including ancestors of an
/// explicitly selected project root. No source-directed traversal is supported.
fn no_symlinks(path: &Path) -> Result<(), String> {
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .map_err(|_| "cannot resolve working directory")?
            .join(path)
    };
    let mut current = PathBuf::new();
    for component in absolute.components() {
        match component {
            #[cfg(windows)]
            Component::Prefix(prefix) => {
                // Do not inspect a bare `C:` or `\\?\C:` prefix: it is not
                // the drive root until the following RootDir is appended.
                // Device namespaces and network shares are outside the first
                // local NTFS host adapter's qualification boundary.
                if !matches!(
                    prefix.kind(),
                    std::path::Prefix::Disk(_) | std::path::Prefix::VerbatimDisk(_)
                ) {
                    return Err("module paths require a local Windows drive".into());
                }
                current.push(component.as_os_str());
                continue;
            }
            Component::CurDir => continue,
            Component::ParentDir => {
                current.pop();
            }
            _ => {
                current.push(component.as_os_str());
            }
        }
        let metadata = fs::symlink_metadata(&current).map_err(|_| "cannot inspect module path")?;
        if metadata.file_type().is_symlink() {
            return Err("module paths and project roots must not traverse symbolic links".into());
        }
        #[cfg(windows)]
        if is_reparse_point(&metadata) {
            return Err("module paths and project roots must not traverse reparse points".into());
        }
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct Identity {
    #[cfg(unix)]
    device: u64,
    #[cfg(unix)]
    inode: u64,
    #[cfg(windows)]
    windows: axeon_platform::FileIdentity,
    #[cfg(not(any(unix, windows)))]
    canonical: PathBuf,
}
#[cfg(not(windows))]
fn identity(metadata: &Metadata, canonical: &Path) -> Identity {
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        let _ = canonical;
        Identity {
            device: metadata.dev(),
            inode: metadata.ino(),
        }
    }
    #[cfg(not(unix))]
    {
        let _ = metadata;
        Identity {
            canonical: canonical.to_path_buf(),
        }
    }
}
#[cfg(windows)]
fn identity(file: &File) -> Result<Identity, String> {
    axeon_platform::identity(file)
        .map(|windows| Identity { windows })
        .map_err(|error| format!("cannot establish source file identity: {error}"))
}

#[cfg(not(windows))]
fn verify_identity(path: &Path, expected: &Identity) -> Result<(), String> {
    let canonical = fs::canonicalize(path).map_err(|_| "source path changed while loading")?;
    let metadata = fs::metadata(path).map_err(|_| "source path changed while loading")?;
    if !metadata.is_file() || identity(&metadata, &canonical) != *expected {
        return Err("source identity changed while loading".into());
    }
    Ok(())
}
#[cfg(windows)]
fn verify_identity(path: &Path, expected: &Identity) -> Result<(), String> {
    let file = windows_source_file(path, false)?;
    if identity(&file)? != *expected {
        return Err("source identity changed while loading".into());
    }
    Ok(())
}
struct Loaded {
    text: String,
    canonical: PathBuf,
    identity: Identity,
}
#[cfg(not(windows))]
fn read_file(path: &Path, strict: bool) -> Result<Loaded, String> {
    if strict {
        no_symlinks(path)?;
    }
    let before = fs::metadata(path).map_err(|_| "cannot open source file")?;
    if !before.is_file() {
        return Err("source must be a regular file".into());
    }
    let canonical = fs::canonicalize(path).map_err(|_| "cannot resolve source file")?;
    let expected = identity(&before, &canonical);
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::fs::OpenOptionsExt;
        // Linux O_NONBLOCK prevents a raced FIFO open from waiting indefinitely;
        // O_NOFOLLOW rejects replacement of a strict module by a symlink.
        options.custom_flags(0x800 | if strict { 0x20000 } else { 0 });
    }
    let file: File = options.open(path).map_err(|_| "cannot open source file")?;
    let opened = file
        .metadata()
        .map_err(|_| "cannot inspect opened source")?;
    if !opened.is_file() || identity(&opened, &canonical) != expected {
        return Err("source identity changed while loading".into());
    }
    let mut bytes = Vec::new();
    (&file)
        .take((MAX_PROJECT_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|_| "cannot read source file")?;
    if bytes.len() > MAX_PROJECT_BYTES {
        return Err("source exceeds 1048576-byte limit".into());
    }
    let after = file.metadata().map_err(|_| "cannot inspect read source")?;
    if opened.len() != after.len()
        || opened.modified().ok() != after.modified().ok()
        || bytes.len() as u64 != after.len()
    {
        return Err("source contents changed while loading".into());
    }
    if strict {
        no_symlinks(path)?;
    }
    verify_identity(path, &expected)?;
    if fs::canonicalize(path).ok().as_ref() != Some(&canonical) {
        return Err("source path changed while loading".into());
    }
    let text = String::from_utf8(bytes).map_err(|_| "source must be valid UTF-8")?;
    Ok(Loaded {
        text,
        canonical,
        identity: expected,
    })
}

#[cfg(windows)]
fn is_reparse_point(metadata: &Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;
    // FILE_ATTRIBUTE_REPARSE_POINT includes junctions and other redirections,
    // not only the symbolic links recognized by FileType::is_symlink().
    metadata.file_attributes() & 0x400 != 0
}

#[cfg(windows)]
fn windows_source_file(path: &Path, strict: bool) -> Result<File, String> {
    use std::os::windows::fs::OpenOptionsExt;
    let absolute = std::path::absolute(path).map_err(|_| "cannot resolve source path")?;
    if !matches!(
        absolute.components().next(),
        Some(Component::Prefix(prefix))
            if matches!(prefix.kind(), std::path::Prefix::Disk(_) | std::path::Prefix::VerbatimDisk(_))
    ) {
        return Err(
            "Windows source reads require a local drive path, not a device or network namespace"
                .into(),
        );
    }
    let before = fs::metadata(&absolute).map_err(|_| "cannot inspect source file")?;
    if !before.is_file() {
        return Err("source must be a regular file".into());
    }
    let mut options = OpenOptions::new();
    // FILE_SHARE_READ deliberately excludes WRITE and DELETE: the held file
    // cannot be overwritten, replaced or renamed during the frozen read.
    // SECURITY_ANONYMOUS avoids lending the compiler's privileges if an
    // unexpected device path reaches the OS open operation.
    options.read(true).share_mode(1).security_qos_flags(0);
    if strict {
        // FILE_FLAG_OPEN_REPARSE_POINT opens the leaf itself for inspection.
        options.custom_flags(0x0020_0000);
    }
    let file = options
        .open(absolute)
        .map_err(|error| format!("cannot open source file for a stable read: {error}"))?;
    let metadata = file
        .metadata()
        .map_err(|_| "cannot inspect opened source")?;
    if !metadata.is_file() {
        return Err("source must be a regular file".into());
    }
    if strict && is_reparse_point(&metadata) {
        return Err("module source must not be a reparse point".into());
    }
    Ok(file)
}

#[cfg(windows)]
fn read_file(path: &Path, strict: bool) -> Result<Loaded, String> {
    if strict {
        no_symlinks(path)?;
    }
    // Keep the first physical file pinned until every comparison is complete.
    // Windows metadata in Rust 1.85 lacks stable physical-identity accessors;
    // obtaining both identities from held handles also avoids a path fallback.
    let before = windows_source_file(path, strict)?;
    let expected = identity(&before)?;
    let canonical = fs::canonicalize(path).map_err(|_| "cannot resolve source file")?;
    let file = windows_source_file(path, strict)?;
    let opened = file
        .metadata()
        .map_err(|_| "cannot inspect opened source")?;
    if identity(&file)? != expected {
        return Err("source identity changed while loading".into());
    }
    let mut bytes = Vec::new();
    (&file)
        .take((MAX_PROJECT_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|_| "cannot read source file")?;
    if bytes.len() > MAX_PROJECT_BYTES {
        return Err("source exceeds 1048576-byte limit".into());
    }
    let after = file.metadata().map_err(|_| "cannot inspect read source")?;
    if opened.len() != after.len()
        || opened.modified().ok() != after.modified().ok()
        || bytes.len() as u64 != after.len()
    {
        return Err("source contents changed while loading".into());
    }
    if strict {
        no_symlinks(path)?;
    }
    verify_identity(path, &expected)?;
    if fs::canonicalize(path).ok().as_ref() != Some(&canonical) {
        return Err("source path changed while loading".into());
    }
    let text = String::from_utf8(bytes).map_err(|_| "source must be valid UTF-8")?;
    Ok(Loaded {
        text,
        canonical,
        identity: expected,
    })
}

#[cfg(all(test, windows))]
mod windows_source_tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static NEXT: AtomicUsize = AtomicUsize::new(0);
    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "axeon-windows-source-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
        fn write(&self, name: &str, source: &str) -> PathBuf {
            let path = self.0.join(name);
            fs::write(&path, source).unwrap();
            path
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn normal_and_canonical_drive_paths_load_the_same_modules() {
        let fixture = Fixture::new();
        let entry = fixture.write(
            "main.ax",
            "import helper; fn main()->i64{return helper::value();}",
        );
        fixture.write("helper.ax", "export fn value()->i64{return 42;}");
        for path in [entry.clone(), fs::canonicalize(entry).unwrap()] {
            let project = Project::load(&path, None).unwrap();
            assert_eq!(project.modules.len(), 2);
            let checked = project.check().unwrap();
            assert_eq!(crate::interpreter::run(&checked, 100_000).result, Ok(42));
        }
    }

    #[test]
    fn imported_hard_link_to_the_entry_is_rejected_as_an_alias() {
        let fixture = Fixture::new();
        let entry = fixture.write("main.ax", "import helper; fn main()->i64{return 0;}");
        fs::hard_link(&entry, fixture.0.join("helper.ax")).unwrap();
        match Project::load(&entry, None).unwrap_err() {
            ProjectError::Diagnostics { diagnostics, .. } => {
                assert!(diagnostics
                    .iter()
                    .any(|error| error.code == "E_MODULE_ALIAS"));
            }
            error => panic!("expected module alias rejection, got {error:?}"),
        }
    }

    #[test]
    fn imports_require_exact_filename_case_on_windows() {
        let fixture = Fixture::new();
        let entry = fixture.write("main.ax", "import helper; fn main()->i64{return 0;}");
        fixture.write("Helper.ax", "export fn value()->i64{return 1;}");
        match Project::load(&entry, None).unwrap_err() {
            ProjectError::Diagnostics { diagnostics, .. } => {
                assert!(diagnostics
                    .iter()
                    .any(|error| error.code == "E_MODULE_NOT_FOUND"));
            }
            error => panic!("expected exact-case rejection, got {error:?}"),
        }
    }

    #[test]
    fn a_source_handle_denies_writing_or_replacement_until_closed() {
        let fixture = Fixture::new();
        let path = fixture.write("main.ax", "fn main()->i64{return 42;}");
        let held = windows_source_file(&path, true).unwrap();
        let expected = identity(&held).unwrap();
        assert!(OpenOptions::new().write(true).open(&path).is_err());
        assert!(fs::rename(&path, fixture.0.join("renamed.ax")).is_err());
        verify_identity(&path, &expected).unwrap();
        assert_eq!(
            fs::read_to_string(&path).unwrap(),
            "fn main()->i64{return 42;}"
        );
        drop(held);
        assert!(OpenOptions::new().write(true).open(&path).is_ok());
    }
}
