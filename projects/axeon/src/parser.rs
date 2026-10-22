//! Recursive descent statements and bounded, left-associative precedence parsing.
use crate::ast::*;
use crate::diagnostic::{Diagnostic, Span};
use crate::lexer::{lex, Token, TokenKind as T, MAX_DIAGNOSTICS};
use std::collections::HashMap;

pub const MAX_NESTING: usize = 64;
pub const MAX_FUNCTIONS: usize = 1024;
pub const MAX_RECORDS: usize = 256;
pub const MAX_RECORD_FIELDS: usize = 32;
pub const MAX_UNION_VARIANTS: usize = 32;
pub const MAX_TRAITS: usize = 64;
pub const MAX_TRAIT_METHODS: usize = 32;
pub const MAX_IMPLEMENTATIONS: usize = 256;
pub const MAX_GENERIC_FUNCTIONS: usize = 128;
pub const MAX_TYPE_PARAMETERS: usize = 8;
pub const MAX_TYPE_BOUNDS: usize = 8;
pub const MAX_GENERIC_NOMINALS: usize = 128;
pub const MAX_APPLICATION_DEPTH: usize = 32;

pub fn parse(source: &str) -> Result<Program, Vec<Diagnostic>> {
    parse_tokens(lex(source)?)
}

/// Parse complete source syntax while retaining application arena ownership.
/// Use check::check_source to prepare generic nominal applications before checking.
pub fn parse_source(source: &str) -> Result<SourceProgram, Vec<Diagnostic>> {
    parse_source_tokens_in_scope(lex(source)?, &HashMap::new())
}

pub(crate) fn parse_tokens(tokens: Vec<Token>) -> Result<Program, Vec<Diagnostic>> {
    parse_tokens_in_scope(tokens, &HashMap::new())
}

pub(crate) struct Headers {
    pub imports: Vec<Import>,
    pub types: Vec<(String, NominalHeader, bool)>,
    pub functions: usize,
    pub generic_functions: usize,
    pub generic_nominals: usize,
    pub exported: bool,
}

/// Validate the complete grammar without resolving externally defined types.
/// This API never exposes its syntax-only AST to checking or code generation.
pub fn validate_syntax(source: &str) -> Result<(), Vec<Diagnostic>> {
    parse_headers(&lex(source)?).map(|_| ())
}

pub(crate) fn parse_headers(tokens: &[Token]) -> Result<Headers, Vec<Diagnostic>> {
    let syntax = parse_source_tokens_mode(tokens.to_vec(), &HashMap::new(), true)?;
    let types: Vec<_> = syntax
        .records
        .iter()
        .map(|record| {
            (
                record.name.clone(),
                nominal_header(record.id, NominalKind::Record, 0, record.name_span),
                record.exported,
            )
        })
        .chain(syntax.unions.iter().map(|union| {
            (
                union.name.clone(),
                nominal_header(union.id, NominalKind::Union, 0, union.name_span),
                union.exported,
            )
        }))
        .chain(syntax.generic_records.iter().map(|template| {
            (
                template.record.name.clone(),
                nominal_header(
                    template.record.id,
                    NominalKind::Record,
                    template.parameters.len(),
                    template.record.name_span,
                ),
                template.record.exported,
            )
        }))
        .chain(syntax.generic_unions.iter().map(|template| {
            (
                template.union.name.clone(),
                nominal_header(
                    template.union.id,
                    NominalKind::Union,
                    template.parameters.len(),
                    template.union.name_span,
                ),
                template.union.exported,
            )
        }))
        .collect();
    Ok(Headers {
        imports: syntax.imports,
        exported: syntax.functions.iter().any(|function| function.exported)
            || syntax
                .generic_functions
                .iter()
                .any(|template| template.function.exported)
            || syntax.traits.iter().any(|contract| contract.exported)
            || types.iter().any(|(_, _, exported)| *exported),
        functions: syntax.functions.len(),
        generic_functions: syntax.generic_functions.len(),
        generic_nominals: syntax.generic_records.len() + syntax.generic_unions.len(),
        types,
    })
}

pub(crate) fn parse_tokens_in_scope(
    tokens: Vec<Token>,
    imported: &HashMap<String, NominalHeader>,
) -> Result<Program, Vec<Diagnostic>> {
    parse_source_tokens_in_scope(tokens, imported)?.resolve_ordinary_annotations()
}

/// Retain source annotations until the project has collected its declarations.
pub(crate) fn parse_source_tokens_in_scope(
    tokens: Vec<Token>,
    imported: &HashMap<String, NominalHeader>,
) -> Result<SourceProgram, Vec<Diagnostic>> {
    parse_source_tokens_mode(tokens, imported, false)
}

fn parse_source_tokens_mode(
    tokens: Vec<Token>,
    imported: &HashMap<String, NominalHeader>,
    syntax_only: bool,
) -> Result<SourceProgram, Vec<Diagnostic>> {
    // Pre-index only top-level declarations, so annotations may precede the
    // declaration without giving equally spelled records in other files identity.
    let mut record_names = imported.clone();
    let mut reserved_type_names = HashMap::new();
    let mut local_names = 0;
    let mut braces = 0usize;
    for (index, token) in tokens.iter().enumerate() {
        if braces == 0
            && matches!(&token.kind, T::Ident(name) if matches!(name.as_str(), "record" | "union" | "trait" | "import"))
        {
            if let Some(Token {
                kind: T::Ident(name),
                span,
            }) = tokens.get(index + 1)
            {
                reserved_type_names.entry(name.clone()).or_insert(*span);
            }
        }
        if braces == 0
            && matches!(&token.kind, T::Ident(name) if name == "record" || name == "union")
        {
            if let (
                Some(Token {
                    kind: T::Ident(name),
                    span,
                }),
                Some(next),
            ) = (tokens.get(index + 1), tokens.get(index + 2))
            {
                if local_names <= MAX_RECORDS && matches!(next.kind, T::LBrace | T::Lt) {
                    let id = RecordId {
                        source: span.source,
                        offset: span.start,
                    };
                    let kind = if matches!(&token.kind, T::Ident(name) if name == "union") {
                        NominalKind::Union
                    } else {
                        NominalKind::Record
                    };
                    let arity = header_arity(&tokens[index + 2..]);
                    record_names.entry(name.clone()).or_insert(NominalHeader {
                        id,
                        kind,
                        arity,
                        name_span: *span,
                    });
                    local_names += 1;
                }
            }
        }
        match token.kind {
            T::LBrace => braces += 1,
            T::RBrace => braces = braces.saturating_sub(1),
            _ => (),
        }
    }
    Parser {
        tokens,
        pos: 0,
        pending: None,
        last_span: Span::default(),
        logical_steps: 0,
        diagnostics: Vec::new(),
        nesting: 0,
        blocks: 0,
        record_names,
        reserved_type_names,
        type_parameters: HashMap::new(),
        record_literals: true,
        syntax_only,
        applications: TypeApplicationsBuilder::new(),
        application_depths: Vec::new(),
        application_depth: 0,
    }
    .program()
}

fn nominal_header(
    id: NominalId,
    kind: NominalKind,
    arity: usize,
    name_span: Span,
) -> NominalHeader {
    NominalHeader {
        id: id.declaration().expect("parser declaration identity"),
        kind,
        arity,
        name_span,
    }
}

// Header scanning discovers identities only. The complete grammar pass below
// diagnoses malformed declarations and all parameter/bound limits.
fn header_arity(tokens: &[Token]) -> usize {
    if !matches!(tokens.first().map(|token| &token.kind), Some(T::Lt)) {
        return 0;
    }
    let mut count = 0;
    let mut parameter = true;
    for token in &tokens[1..] {
        match token.kind {
            T::Gt | T::Shr | T::Ge | T::LBrace | T::RBrace | T::Eof => break,
            T::Comma => parameter = true,
            _ if parameter => {
                count += 1;
                parameter = false;
                if count > MAX_TYPE_PARAMETERS {
                    break;
                }
            }
            _ => {}
        }
    }
    count
}

struct Parser {
    tokens: Vec<Token>,
    pos: usize,
    pending: Option<Token>,
    last_span: Span,
    logical_steps: usize,
    diagnostics: Vec<Diagnostic>,
    nesting: usize,
    blocks: usize,
    record_names: HashMap<String, NominalHeader>,
    reserved_type_names: HashMap<String, Span>,
    type_parameters: HashMap<String, TypeParameterId>,
    record_literals: bool,
    syntax_only: bool,
    applications: TypeApplicationsBuilder,
    application_depths: Vec<usize>,
    application_depth: usize,
}
struct CursorMark {
    pos: usize,
    pending: Option<Token>,
    last_span: Span,
    logical_steps: usize,
}
struct ParsedExpr {
    expr: Expr,
    depth: usize,
}
type ParseResult<V> = Result<V, ()>;

impl Parser {
    fn current(&self) -> &Token {
        self.pending.as_ref().unwrap_or(&self.tokens[self.pos])
    }
    fn peek(&self, distance: usize) -> Option<&Token> {
        if let Some(pending) = &self.pending {
            if distance == 0 {
                Some(pending)
            } else {
                self.tokens.get(self.pos + distance - 1)
            }
        } else {
            self.tokens.get(self.pos + distance)
        }
    }
    fn mark(&self) -> CursorMark {
        CursorMark {
            pos: self.pos,
            pending: self.pending.clone(),
            last_span: self.last_span,
            logical_steps: self.logical_steps,
        }
    }
    fn restore(&mut self, mark: CursorMark) {
        self.pos = mark.pos;
        self.pending = mark.pending;
        self.last_span = mark.last_span;
        self.logical_steps = mark.logical_steps;
    }
    fn at(&self, kind: &T) -> bool {
        std::mem::discriminant(&self.current().kind) == std::mem::discriminant(kind)
    }
    fn bump(&mut self) -> Token {
        let token = if let Some(token) = self.pending.take() {
            token
        } else {
            let token = self.tokens[self.pos].clone();
            if !matches!(token.kind, T::Eof) {
                self.pos += 1;
            }
            token
        };
        if !matches!(token.kind, T::Eof) {
            self.logical_steps += 1;
        }
        self.last_span = token.span;
        token
    }
    fn at_type_close(&self) -> bool {
        matches!(self.current().kind, T::Gt | T::Shr | T::Ge)
    }
    fn expect_type_close(&mut self, description: &str) -> ParseResult<Token> {
        // Split only while closing explicit types. The lexer and formatter keep
        // physical >>/>= tokens; each virtual half retains its own byte span.
        if matches!(self.current().kind, T::Shr | T::Ge) {
            let token = self.bump();
            let middle = token.span.start + 1;
            self.pending = Some(Token {
                kind: if matches!(token.kind, T::Shr) {
                    T::Gt
                } else {
                    T::Eq
                },
                span: Span::in_source(token.span.source, middle, token.span.end),
            });
            let first = Token {
                kind: T::Gt,
                span: Span::in_source(token.span.source, token.span.start, middle),
            };
            self.last_span = first.span;
            Ok(first)
        } else {
            self.expect(&T::Gt, description)
        }
    }
    fn eat(&mut self, kind: &T) -> Option<Token> {
        if self.at(kind) {
            Some(self.bump())
        } else {
            None
        }
    }
    fn error(&mut self, code: &'static str, span: Span, message: impl Into<String>) {
        if self.diagnostics.len() < MAX_DIAGNOSTICS {
            self.diagnostics.push(Diagnostic::new(code, span, message));
        }
    }
    fn expected(&mut self, description: &str) {
        self.error(
            "E_PARSE_EXPECTED",
            self.current().span,
            format!("expected {description}"),
        );
    }
    fn expect(&mut self, kind: &T, description: &str) -> ParseResult<Token> {
        if self.at(kind) {
            Ok(self.bump())
        } else {
            self.expected(description);
            Err(())
        }
    }
    fn field_path(&mut self) -> ParseResult<(String, Span)> {
        let (mut path, mut span) = self.identifier()?;
        let mut depth = 1;
        while self.eat(&T::Dot).is_some() {
            let (field, next) = self.identifier()?;
            depth += 1;
            self.check_depth(depth, span.join(next))?;
            path.push('.');
            path.push_str(&field);
            span = span.join(next);
        }
        Ok((path, span))
    }

    fn identifier(&mut self) -> ParseResult<(String, Span)> {
        if matches!(self.current().kind, T::Ident(_)) {
            let token = self.bump();
            if let T::Ident(name) = token.kind {
                return Ok((name, token.span));
            }
        }
        self.expected("an ASCII identifier");
        Err(())
    }
    fn ty(&mut self) -> ParseResult<Type> {
        if self.eat(&T::I64).is_some() {
            Ok(Type::I64)
        } else if self.eat(&T::F64).is_some() {
            Ok(Type::F64)
        } else if self.eat(&T::Bool).is_some() {
            Ok(Type::Bool)
        } else if self.eat(&T::LParen).is_some() {
            self.expect(&T::RParen, "')' in the unit type ()")?;
            Ok(Type::Unit)
        } else if self.eat(&T::Tensor).is_some() {
            Ok(Type::Tensor)
        } else if self.eat(&T::TensorError).is_some() {
            Ok(Type::TensorError)
        } else if matches!(self.current().kind, T::Ident(_)) {
            let (mut name, mut span) = self.identifier()?;
            let qualified = self.eat(&T::DoubleColon).is_some();
            if qualified {
                let (member, end) = self.identifier()?;
                name.push_str("::");
                name.push_str(&member);
                span = span.join(end);
            }
            if let Some(header) = self.record_names.get(&name).cloned() {
                if header.arity != 0 {
                    self.error(
                        "G003",
                        span,
                        "generic nominal type requires all explicit type arguments",
                    );
                    return Err(());
                }
                Ok(match header.kind {
                    NominalKind::Record => Type::Record(header.id.into()),
                    NominalKind::Union => Type::Union(header.id.into()),
                })
            } else if qualified && self.syntax_only {
                // A grammar-only placeholder, discarded by parse_headers. Strict
                // compiler parsing always resolves the actual nominal identity.
                Ok(Type::I64)
            } else {
                self.error(
                    "E_PARSE_TYPE",
                    span,
                    format!("unknown module-local record or union type `{name}`"),
                );
                Err(())
            }
        } else if self.eat(&T::LBracket).is_some() {
            let element = self.element_type()?;
            self.expect(&T::Semicolon, "';' before the array length")?;
            let token = self.current().clone();
            let length = if let T::Integer(digits) = token.kind {
                self.bump();
                digits.parse::<usize>().map_err(|_| {
                    self.error(
                        "E_ARRAY_LENGTH_RANGE",
                        token.span,
                        "array length exceeds the supported integer range",
                    );
                })?
            } else {
                self.expected("a decimal integer array length");
                return Err(());
            };
            self.expect(&T::RBracket, "']' after the array length")?;
            Ok(Type::Array { element, length })
        } else if self.eat(&T::Amp).is_some() {
            let mutable = self.eat(&T::Mut).is_some();
            if self.eat(&T::Tensor).is_some() {
                return Ok(Type::TensorView { mutable });
            }
            self.expect(&T::LBracket, "'[' to begin a slice type")?;
            let element = self.element_type()?;
            self.expect(&T::RBracket, "']' after the slice element type")?;
            Ok(Type::Slice { element, mutable })
        } else {
            self.error(
                "E_PARSE_TYPE",
                self.current().span,
                "expected i64, f64, bool, (), an i64/f64 array or slice, tensor, tensor view, tensor_error, or a module-local record or union type",
            );
            Err(())
        }
    }
    fn type_expr(&mut self) -> ParseResult<TypeExpr> {
        if let T::Ident(name) = &self.current().kind {
            if let Some(&id) = self.type_parameters.get(name) {
                self.bump();
                return Ok(TypeExpr::Parameter(id));
            }
        }
        if matches!(self.current().kind, T::Ident(_)) {
            let (mut name, mut span) = self.identifier()?;
            if self.eat(&T::DoubleColon).is_some() {
                let (member, end) = self.identifier()?;
                name.push_str("::");
                name.push_str(&member);
                span = span.join(end);
            }
            if self.at(&T::Lt) {
                let args = self.nominal_type_arguments()?;
                return self.nominal_application(&name, span.join(self.last_span), args);
            }
            if let Some(header) = self.record_names.get(&name).cloned() {
                if header.arity != 0 {
                    self.error(
                        "G003",
                        span,
                        "generic nominal type requires all explicit type arguments",
                    );
                    return Err(());
                }
                return Ok(TypeExpr::Concrete(match header.kind {
                    NominalKind::Record => Type::Record(header.id.into()),
                    NominalKind::Union => Type::Union(header.id.into()),
                }));
            }
            if self.syntax_only && name.contains("::") {
                return Ok(TypeExpr::Concrete(Type::I64));
            }
            self.error(
                "E_PARSE_TYPE",
                span,
                format!("unknown module-local record or union type `{name}`"),
            );
            return Err(());
        }
        self.ty().map(TypeExpr::Concrete)
    }

    fn nominal_application(
        &mut self,
        name: &str,
        span: Span,
        arguments: Vec<TypeExpr>,
    ) -> ParseResult<TypeExpr> {
        let Some(header) = self.record_names.get(name).cloned() else {
            if self.syntax_only && name.contains("::") {
                return Ok(TypeExpr::Concrete(Type::I64));
            }
            self.error(
                "E_PARSE_TYPE",
                span,
                format!("unknown module-local record or union type `{name}`"),
            );
            return Err(());
        };
        if header.arity == 0 || arguments.len() != header.arity {
            self.error(
                "G003",
                span,
                format!(
                    "nominal type `{name}` requires {} explicit type arguments",
                    header.arity
                ),
            );
            return Err(());
        }
        let mut depth = 1;
        for argument in &arguments {
            if let TypeExpr::Application(id) = argument {
                if self.applications.get(*id).is_none() {
                    self.error(
                        "G007",
                        span,
                        "type argument belongs to a different source arena",
                    );
                    return Err(());
                }
                let Some(child_depth) = self.application_depths.get(id.index as usize) else {
                    self.error("G007", span, "type argument depth is unavailable");
                    return Err(());
                };
                depth = depth.max(child_depth + 1);
            }
        }
        if depth > MAX_APPLICATION_DEPTH {
            self.error("G002", span, "type applications exceed depth 32");
            return Err(());
        }
        let id = self
            .applications
            .insert(TypeApplication {
                declaration: header.id,
                kind: header.kind,
                arguments: arguments.into_boxed_slice(),
            })
            .ok_or_else(|| {
                self.error(
                    "G002",
                    span,
                    "source application arena exceeds its bounded node or argument limit",
                )
            })?;
        self.application_depths.push(depth);
        Ok(TypeExpr::Application(id))
    }

    fn nominal_type_arguments(&mut self) -> ParseResult<Vec<TypeExpr>> {
        if self.application_depth >= MAX_APPLICATION_DEPTH {
            self.error(
                "G002",
                self.current().span,
                "type applications exceed depth 32",
            );
            return Err(());
        }
        self.application_depth += 1;
        let result = self.type_arguments();
        self.application_depth -= 1;
        result
    }
    fn type_arguments(&mut self) -> ParseResult<Vec<TypeExpr>> {
        self.expect(&T::Lt, "'<' before explicit type arguments")?;
        let mut arguments = Vec::new();
        loop {
            if arguments.len() >= MAX_TYPE_PARAMETERS {
                self.error(
                    "G003",
                    self.current().span,
                    "a generic call supports at most 8 explicit type arguments",
                );
                return Err(());
            }
            arguments.push(self.type_expr()?);
            if self.eat(&T::Comma).is_none() || self.at_type_close() {
                break;
            }
        }
        self.expect_type_close("'>' after explicit type arguments")?;
        Ok(arguments)
    }

    fn element_type(&mut self) -> ParseResult<ElementType> {
        if self.eat(&T::I64).is_some() {
            Ok(ElementType::I64)
        } else if self.eat(&T::F64).is_some() {
            Ok(ElementType::F64)
        } else {
            self.expected("i64 or f64 as the aggregate element type");
            Err(())
        }
    }
    fn enter(&mut self) -> ParseResult<()> {
        if self.nesting >= MAX_NESTING {
            self.error(
                "E_NESTING_LIMIT",
                self.current().span,
                format!("syntax nesting exceeds the limit of {MAX_NESTING}"),
            );
            Err(())
        } else {
            self.nesting += 1;
            Ok(())
        }
    }
    fn program(mut self) -> Result<SourceProgram, Vec<Diagnostic>> {
        let mut functions = Vec::new();
        let mut generic_functions = Vec::new();
        let mut imports = Vec::new();
        let mut records = Vec::new();
        let mut unions = Vec::new();
        let mut generic_records = Vec::new();
        let mut generic_unions = Vec::new();
        let mut traits = Vec::new();
        let mut implementations = Vec::new();
        let mut attempted_functions = 0;
        let mut attempted_generics = 0;
        let mut attempted_records = 0;
        let mut attempted_generic_nominals = 0;
        while !self.at(&T::Eof) && self.diagnostics.len() < MAX_DIAGNOSTICS {
            let exported = self.contextual("export");
            if exported {
                self.bump();
            }
            if self.contextual("trait") {
                if traits.len() >= MAX_TRAITS {
                    self.error(
                        "TR001",
                        self.current().span,
                        "program exceeds 64 trait declarations",
                    );
                    break;
                }
                match self.trait_declaration() {
                    Ok(mut contract) => {
                        contract.exported = exported;
                        traits.push(contract);
                    }
                    Err(()) => self.synchronize_function(),
                }
            } else if self.contextual("impl") && !exported {
                if implementations.len() >= MAX_IMPLEMENTATIONS {
                    self.error(
                        "TR001",
                        self.current().span,
                        "program exceeds 256 concrete implementations",
                    );
                    break;
                }
                match self.implementation() {
                    Ok(implementation) => implementations.push(implementation),
                    Err(()) => self.synchronize_function(),
                }
            } else if self.contextual("record") || self.contextual("union") {
                attempted_records += 1;
                if attempted_records > MAX_RECORDS {
                    self.error(
                        "R001",
                        self.current().span,
                        format!("program exceeds {MAX_RECORDS} record and union declarations"),
                    );
                    break;
                }
                if matches!(self.peek(2).map(|token| &token.kind), Some(T::Lt)) {
                    attempted_generic_nominals += 1;
                    if attempted_generic_nominals > MAX_GENERIC_NOMINALS {
                        self.error(
                            "G002",
                            self.current().span,
                            "program exceeds 128 generic nominal declarations",
                        );
                        break;
                    }
                }
                if self.contextual("union") {
                    match self.union() {
                        Ok((mut union, parameters)) => {
                            union.exported = exported;
                            if parameters.is_empty() {
                                unions.push(union);
                            } else {
                                generic_unions.push(GenericUnion { parameters, union });
                            }
                        }
                        Err(()) => self.synchronize_function(),
                    }
                } else {
                    match self.record() {
                        Ok((mut record, parameters)) => {
                            record.exported = exported;
                            if parameters.is_empty() {
                                records.push(record);
                            } else {
                                generic_records.push(GenericRecord { parameters, record });
                            }
                        }
                        Err(()) => self.synchronize_function(),
                    }
                }
            } else if self.contextual("import") && !exported {
                let start = self.bump().span;
                let result = (|| {
                    let (name, _) = self.identifier()?;
                    let end = self
                        .expect(&T::Semicolon, "';' after the module name")?
                        .span;
                    Ok(Import {
                        name,
                        span: start.join(end),
                    })
                })();
                match result {
                    Ok(import) => imports.push(import),
                    Err(()) => self.synchronize_function(),
                }
            } else if self.at(&T::Fn) {
                if matches!(self.peek(2).map(|token| &token.kind), Some(T::Lt)) {
                    attempted_generics += 1;
                    if attempted_generics > MAX_GENERIC_FUNCTIONS {
                        self.error(
                            "G002",
                            self.current().span,
                            "program exceeds 128 generic function declarations",
                        );
                        break;
                    }
                    match self.generic_function() {
                        Ok(mut template) => {
                            template.function.exported = exported;
                            generic_functions.push(template);
                        }
                        Err(()) => self.synchronize_function(),
                    }
                    continue;
                }
                attempted_functions += 1;
                if attempted_functions > MAX_FUNCTIONS {
                    self.error(
                        "E_FUNCTION_LIMIT",
                        self.current().span,
                        format!("program exceeds the limit of {MAX_FUNCTIONS} functions"),
                    );
                    break;
                }
                if let Ok(mut function) = self.function() {
                    function.exported = exported;
                    functions.push(function);
                } else {
                    self.synchronize_function();
                }
            } else {
                self.error(
                    "E_PARSE_TOP_LEVEL",
                    self.current().span,
                    "expected a function, module import, record, or union declaration",
                );
                self.synchronize_function();
            }
        }
        if self.diagnostics.is_empty() {
            Ok(Program {
                imports,
                records,
                unions,
                traits,
                implementations,
                functions,
                generic_functions,
                generic_records,
                generic_unions,
                applications: self.applications.finish(),
            })
        } else {
            Err(self.diagnostics)
        }
    }
    fn contextual(&self, name: &str) -> bool {
        matches!(&self.current().kind, T::Ident(value) if value == name)
    }
    fn trait_type(&mut self) -> ParseResult<TraitType<TypeExpr>> {
        if self.contextual("Self") {
            self.bump();
            Ok(TraitType::SelfType)
        } else {
            self.type_expr().map(TraitType::Concrete)
        }
    }
    fn trait_declaration(&mut self) -> ParseResult<Trait<TypeExpr>> {
        self.bump();
        let (name, name_span) = self.identifier()?;
        self.expect(&T::LBrace, "'{' before trait methods")?;
        let mut methods = Vec::new();
        while !self.at(&T::RBrace) {
            if methods.len() >= MAX_TRAIT_METHODS {
                self.error(
                    "TR001",
                    self.current().span,
                    "a trait supports 1..=32 methods",
                );
                return Err(());
            }
            self.expect(&T::Fn, "fn before a trait method signature")?;
            let (name, name_span) = self.identifier()?;
            self.expect(&T::LParen, "'(' after the method name")?;
            let mut params = Vec::new();
            while !self.at(&T::RParen) {
                if params.len() >= crate::check::MAX_PARAMS {
                    self.error(
                        "T014",
                        self.current().span,
                        "trait method exceeds 32 parameters",
                    );
                    return Err(());
                }
                let (name, name_span) = self.identifier()?;
                self.expect(&T::Colon, "':' after a parameter name")?;
                let ty = self.trait_type()?;
                params.push(TraitParam {
                    name,
                    name_span,
                    ty,
                });
                if self.eat(&T::Comma).is_none() {
                    break;
                }
            }
            self.expect(&T::RParen, "')' after trait method parameters")?;
            self.expect(&T::Arrow, "'->' and an explicit return type")?;
            let return_type = self.trait_type()?;
            self.expect(&T::Semicolon, "';' after a trait method signature")?;
            methods.push(TraitMethod {
                name,
                name_span,
                params,
                return_type,
            });
        }
        self.bump();
        Ok(Trait {
            id: RecordId {
                source: name_span.source,
                offset: name_span.start,
            },
            exported: false,
            name,
            name_span,
            methods,
        })
    }
    fn implementation(&mut self) -> ParseResult<Implementation<TypeExpr>> {
        self.bump();
        let (mut trait_name, mut trait_span) = self.identifier()?;
        if self.eat(&T::DoubleColon).is_some() {
            let (name, span) = self.identifier()?;
            trait_name.push_str("::");
            trait_name.push_str(&name);
            trait_span = trait_span.join(span);
        }
        if !self.contextual("for") {
            self.expected("for before the concrete implementation type");
            return Err(());
        }
        self.bump();
        let target = self.type_expr()?;
        self.expect(&T::LBrace, "'{' before implementation method bindings")?;
        let mut bindings = Vec::new();
        while !self.at(&T::RBrace) {
            if bindings.len() >= MAX_TRAIT_METHODS {
                self.error(
                    "TR001",
                    self.current().span,
                    "an implementation supports at most 32 method bindings",
                );
                return Err(());
            }
            let (method, method_span) = self.identifier()?;
            self.expect(&T::Eq, "'=' before a local function binding")?;
            let (function, function_span) = self.identifier()?;
            self.expect(&T::Semicolon, "';' after a method binding")?;
            bindings.push(MethodBinding {
                method,
                method_span,
                function,
                function_span,
            });
        }
        self.bump();
        Ok(Implementation {
            trait_name,
            trait_span,
            target,
            bindings,
        })
    }
    fn record(&mut self) -> ParseResult<(Record<TypeExpr>, Vec<TypeParameter>)> {
        self.bump();
        let (name, name_span) = self.identifier()?;
        let previous = std::mem::take(&mut self.type_parameters);
        let result = (|| {
            let parameters = if self.at(&T::Lt) {
                self.parameters(RecordId {
                    source: name_span.source,
                    offset: name_span.start,
                })?
            } else {
                Vec::new()
            };
            self.expect(&T::LBrace, "'{' before record fields")?;
            let mut fields = Vec::new();
            while !self.at(&T::RBrace) {
                if fields.len() >= MAX_RECORD_FIELDS {
                    self.error(
                        "R002",
                        self.current().span,
                        format!("a record requires 1..={MAX_RECORD_FIELDS} fields"),
                    );
                    return Err(());
                }
                let (name, name_span) = self.identifier()?;
                self.expect(&T::Colon, "':' before a record field type")?;
                let ty = self.type_expr()?;
                fields.push(RecordField {
                    name,
                    name_span,
                    ty,
                });
                if self.eat(&T::Comma).is_none() {
                    break;
                }
            }
            self.expect(&T::RBrace, "'}' after record fields")?;
            Ok((
                Record {
                    exported: false,
                    id: RecordId {
                        source: name_span.source,
                        offset: name_span.start,
                    }
                    .into(),
                    name,
                    name_span,
                    fields,
                },
                parameters,
            ))
        })();
        self.type_parameters = previous;
        result
    }
    fn union(&mut self) -> ParseResult<(Union<TypeExpr>, Vec<TypeParameter>)> {
        self.bump();
        let (name, name_span) = self.identifier()?;
        let previous = std::mem::take(&mut self.type_parameters);
        let result = (|| {
            let parameters = if self.at(&T::Lt) {
                self.parameters(RecordId {
                    source: name_span.source,
                    offset: name_span.start,
                })?
            } else {
                Vec::new()
            };
            self.expect(&T::LBrace, "'{' before union variants")?;
            let mut variants = Vec::new();
            while !self.at(&T::RBrace) {
                if variants.len() >= MAX_UNION_VARIANTS {
                    self.error(
                        "U001",
                        self.current().span,
                        "a union requires 1..=32 variants",
                    );
                    return Err(());
                }
                let (name, name_span) = self.identifier()?;
                self.expect(
                    &T::LBrace,
                    "'{' before variant fields (including empty variants)",
                )?;
                let mut fields = Vec::new();
                while !self.at(&T::RBrace) {
                    if fields.len() >= MAX_RECORD_FIELDS {
                        self.error(
                            "U002",
                            self.current().span,
                            "a union variant supports at most 32 fields",
                        );
                        return Err(());
                    }
                    let (name, name_span) = self.identifier()?;
                    self.expect(&T::Colon, "':' before a variant field type")?;
                    let ty = self.type_expr()?;
                    fields.push(RecordField {
                        name,
                        name_span,
                        ty,
                    });
                    if self.eat(&T::Comma).is_none() {
                        break;
                    }
                }
                self.expect(&T::RBrace, "'}' after variant fields")?;
                variants.push(UnionVariant {
                    name,
                    name_span,
                    fields,
                });
                if self.eat(&T::Comma).is_none() {
                    break;
                }
            }
            self.expect(&T::RBrace, "'}' after union variants")?;
            Ok((
                Union {
                    exported: false,
                    id: RecordId {
                        source: name_span.source,
                        offset: name_span.start,
                    }
                    .into(),
                    name,
                    name_span,
                    variants,
                },
                parameters,
            ))
        })();
        self.type_parameters = previous;
        result
    }

    fn match_statement<A>(
        &mut self,
        annotation: fn(&mut Self) -> ParseResult<A>,
    ) -> ParseResult<StmtKind<A>> {
        self.bump();
        let (name, name_span) = self.identifier()?;
        self.expect(&T::LBrace, "'{' after a bare union owner in match")?;
        let mut arms = Vec::new();
        while !self.at(&T::RBrace) {
            if arms.len() >= MAX_UNION_VARIANTS {
                self.error(
                    "U001",
                    self.current().span,
                    "a match supports at most 32 variant arms",
                );
                return Err(());
            }
            let (mut union_name, mut union_span) = self.identifier()?;
            self.expect(
                &T::DoubleColon,
                "'::' before an explicit union variant pattern",
            )?;
            let mut application = None;
            let (variant, variant_span) = if self.at(&T::Lt) {
                let args = self.nominal_type_arguments()?;
                union_span = union_span.join(self.last_span);
                application = Some(self.nominal_application(&union_name, union_span, args)?);
                self.expect(&T::DoubleColon, "'::' before a generic union variant")?;
                self.identifier()?
            } else {
                let (member, member_span) = self.identifier()?;
                if self.eat(&T::DoubleColon).is_some() {
                    union_name.push_str("::");
                    union_name.push_str(&member);
                    union_span = union_span.join(member_span);
                    if self.at(&T::Lt) {
                        let args = self.nominal_type_arguments()?;
                        union_span = union_span.join(self.last_span);
                        application =
                            Some(self.nominal_application(&union_name, union_span, args)?);
                        self.expect(&T::DoubleColon, "'::' before a generic union variant")?;
                    }
                    self.identifier()?
                } else {
                    (member, member_span)
                }
            };
            self.expect(&T::LBrace, "'{' before explicit pattern fields")?;
            let mut fields = Vec::new();
            while !self.at(&T::RBrace) {
                if fields.len() >= MAX_RECORD_FIELDS {
                    self.error(
                        "U002",
                        self.current().span,
                        "a variant pattern supports at most 32 fields",
                    );
                    return Err(());
                }
                let (name, name_span) = self.identifier()?;
                self.expect(&T::Colon, "':' before an explicit scalar pattern binding")?;
                let (binding, binding_span) = self.identifier()?;
                fields.push(PatternField {
                    name,
                    name_span,
                    binding,
                    binding_span,
                });
                if self.eat(&T::Comma).is_none() {
                    break;
                }
            }
            self.expect(&T::RBrace, "'}' after pattern fields")?;
            self.expect(&T::FatArrow, "'=>' before a match arm block")?;
            let body = self.block(annotation)?;
            arms.push(MatchArm {
                union_name,
                union_span,
                application,
                variant,
                variant_span,
                fields,
                body,
            });
            if self.eat(&T::Comma).is_none() {
                break;
            }
        }
        self.expect(&T::RBrace, "'}' after match arms")?;
        Ok(StmtKind::Match {
            name,
            name_span,
            arms,
        })
    }

    fn union_constructor(
        &mut self,
        name: String,
        variant: String,
        variant_span: Span,
        span: Span,
        application: Option<TypeExpr>,
    ) -> ParseResult<ParsedExpr> {
        self.expect(&T::LBrace, "'{' before variant initializers")?;
        let mut fields = Vec::new();
        let mut depth = 1;
        while !self.at(&T::RBrace) {
            if fields.len() >= MAX_RECORD_FIELDS {
                self.error(
                    "U002",
                    self.current().span,
                    "a variant constructor supports at most 32 fields",
                );
                return Err(());
            }
            let (name, name_span) = self.identifier()?;
            self.expect(&T::Colon, "':' before an explicit variant field value")?;
            let value = self.expression(1)?;
            depth = depth.max(value.depth + 1);
            self.check_depth(depth, span.join(value.expr.span))?;
            fields.push(FieldInitializer {
                name,
                name_span,
                value: value.expr,
            });
            if self.eat(&T::Comma).is_none() {
                break;
            }
        }
        let end = self
            .expect(&T::RBrace, "'}' after variant construction")?
            .span;
        Ok(ParsedExpr {
            expr: Expr {
                kind: ExprKind::Union {
                    name,
                    application,
                    variant,
                    variant_span,
                    fields,
                },
                span: span.join(end),
            },
            depth,
        })
    }

    fn synchronize_function(&mut self) {
        while !self.at(&T::Fn)
            && !self.at(&T::Eof)
            && !self.contextual("import")
            && !self.contextual("export")
            && !self.contextual("record")
            && !self.contextual("union")
            && !self.contextual("trait")
            && !self.contextual("impl")
        {
            self.bump();
        }
    }
    fn function(&mut self) -> ParseResult<Function<TypeExpr>> {
        let start = self.expect(&T::Fn, "fn")?.span;
        let (name, name_span) = self.identifier()?;
        self.function_body(start, name, name_span, Self::type_expr)
    }

    fn generic_function(&mut self) -> ParseResult<GenericFunction> {
        let start = self.expect(&T::Fn, "fn")?.span;
        let (name, name_span) = self.identifier()?;
        let binder = RecordId {
            source: name_span.source,
            offset: name_span.start,
        };
        let previous = std::mem::take(&mut self.type_parameters);
        // Restore the declaration-local environment on every recovery path.
        let result = (|| {
            let parameters = self.parameters(binder)?;
            let function = self.function_body(start, name, name_span, Self::type_expr)?;
            Ok(GenericFunction {
                parameters,
                function,
            })
        })();
        self.type_parameters = previous;
        result
    }

    fn parameters(&mut self, binder: RecordId) -> ParseResult<Vec<TypeParameter>> {
        self.expect(&T::Lt, "'<' before type parameters")?;
        let mut parameters = Vec::new();
        loop {
            if parameters.len() >= MAX_TYPE_PARAMETERS {
                self.error(
                    "G002",
                    self.current().span,
                    "a generic declaration supports at most 8 type parameters",
                );
                return Err(());
            }
            let (parameter, parameter_span) = self.identifier()?;
            if parameter == "Self"
                || self.reserved_type_names.contains_key(&parameter)
                || self.record_names.contains_key(&parameter)
                || self.type_parameters.contains_key(&parameter)
            {
                self.error(
                        "G001",
                        parameter_span,
                        format!("type parameter `{parameter}` duplicates or shadows a type, trait, import alias, or type parameter"),
                    );
                return Err(());
            }
            self.type_parameters.insert(
                parameter.clone(),
                TypeParameterId {
                    binder,
                    index: parameters.len(),
                },
            );
            let mut bounds = Vec::new();
            if self.eat(&T::Colon).is_some() {
                loop {
                    if bounds.len() >= MAX_TYPE_BOUNDS {
                        self.error(
                            "G002",
                            self.current().span,
                            "a type parameter supports at most 8 trait bounds",
                        );
                        return Err(());
                    }
                    let (mut name, mut span) = self.identifier()?;
                    if self.eat(&T::DoubleColon).is_some() {
                        let (member, end) = self.identifier()?;
                        name.push_str("::");
                        name.push_str(&member);
                        span = span.join(end);
                    }
                    bounds.push(TraitBound { name, span });
                    if self.eat(&T::Plus).is_none() {
                        break;
                    }
                }
            }
            parameters.push(TypeParameter {
                name: parameter,
                name_span: parameter_span,
                bounds,
            });
            if self.eat(&T::Comma).is_none() || self.at(&T::Gt) {
                break;
            }
        }
        self.expect(&T::Gt, "'>' after type parameters")?;
        Ok(parameters)
    }

    fn function_body<A>(
        &mut self,
        start: Span,
        name: String,
        name_span: Span,
        annotation: fn(&mut Self) -> ParseResult<A>,
    ) -> ParseResult<Function<A>> {
        self.expect(&T::LParen, "'(' after the function name")?;
        let mut params = Vec::new();
        if !self.at(&T::RParen) {
            loop {
                let (name, name_span) = self.identifier()?;
                self.expect(&T::Colon, "':' after a parameter name")?;
                let ty = annotation(self)?;
                params.push(Param {
                    name,
                    name_span,
                    ty,
                });
                if self.eat(&T::Comma).is_none() || self.at(&T::RParen) {
                    break;
                }
            }
        }
        self.expect(&T::RParen, "')' after parameters")?;
        self.expect(&T::Arrow, "'->' and an explicit return type")?;
        let return_type = annotation(self)?;
        let body = self.block(annotation)?;
        let span = start.join(body.span);
        Ok(Function {
            exported: false,
            name,
            name_span,
            params,
            return_type,
            body,
            span,
        })
    }
    fn block<A>(&mut self, annotation: fn(&mut Self) -> ParseResult<A>) -> ParseResult<Block<A>> {
        self.enter()?;
        self.blocks += 1;
        let result = self.block_inner(annotation);
        self.blocks -= 1;
        self.nesting -= 1;
        result
    }
    fn block_inner<A>(
        &mut self,
        annotation: fn(&mut Self) -> ParseResult<A>,
    ) -> ParseResult<Block<A>> {
        let start = self.expect(&T::LBrace, "'{' to begin a block")?.span;
        let mut stmts = Vec::new();
        while !self.at(&T::RBrace) && !self.at(&T::Eof) && self.diagnostics.len() < MAX_DIAGNOSTICS
        {
            if self.at(&T::Fn) {
                self.expected("'}' before the next function");
                return Err(());
            }
            let before = self.logical_steps;
            match self.statement(annotation) {
                Ok(stmt) => stmts.push(stmt),
                Err(()) => self.synchronize_statement(before),
            }
        }
        let end = self.expect(&T::RBrace, "'}' to end the block")?.span;
        Ok(Block {
            stmts,
            span: start.join(end),
        })
    }
    fn synchronize_statement(&mut self, before: usize) {
        let mut braces = 0usize;
        loop {
            if self.at(&T::Eof) || self.at(&T::Fn) {
                return;
            }
            if braces == 0 {
                if self.at(&T::RBrace) {
                    return;
                }
                if self.at(&T::Semicolon) {
                    self.bump();
                    return;
                }
                if self.logical_steps > before
                    && matches!(
                        self.current().kind,
                        T::Let
                            | T::Return
                            | T::Print
                            | T::If
                            | T::While
                            | T::Break
                            | T::Continue
                            | T::TensorScope
                    )
                {
                    return;
                }
            }
            match self.current().kind {
                T::LBrace => braces += 1,
                T::RBrace => braces = braces.saturating_sub(1),
                _ => (),
            }
            self.bump();
        }
    }
    fn statement<A>(
        &mut self,
        annotation: fn(&mut Self) -> ParseResult<A>,
    ) -> ParseResult<Stmt<A>> {
        let start = self.current().span;
        let kind = match self.current().kind {
            T::Ident(_)
                if self.contextual("match")
                    && matches!(self.peek(1).map(|token| &token.kind), Some(T::Ident(_))) =>
            {
                self.match_statement(annotation)?
            }
            T::TensorScope => {
                self.bump();
                self.expect(&T::LParen, "'(' after tensor_scope")?;
                let budget = self.expression(1)?.expr;
                self.expect(&T::RParen, "')' after the tensor budget")?;
                let body = self.block(annotation)?;
                self.expect(&T::Catch, "catch after the tensor scope body")?;
                self.expect(&T::LParen, "'(' before the tensor error binding")?;
                let (error_name, error_span) = self.identifier()?;
                self.expect(&T::Colon, "':' before tensor_error")?;
                self.expect(&T::TensorError, "tensor_error as the catch binding type")?;
                self.expect(&T::RParen, "')' after the tensor error binding")?;
                let handler = self.block(annotation)?;
                StmtKind::TensorScope {
                    budget,
                    body,
                    error_name,
                    error_span,
                    handler,
                }
            }
            T::Let => {
                self.bump();
                let mutable = self.eat(&T::Mut).is_some();
                let (name, name_span) = self.identifier()?;
                self.expect(&T::Colon, "':' and an explicit variable type")?;
                let ty = annotation(self)?;
                self.expect(&T::Eq, "'=' in a variable declaration")?;
                let value = self.expression(1)?.expr;
                self.expect(&T::Semicolon, "';' after a variable declaration")?;
                StmtKind::Let {
                    name,
                    name_span,
                    mutable,
                    ty,
                    value,
                }
            }
            T::If => {
                self.bump();
                let condition = self.condition()?;
                let then_block = self.block(annotation)?;
                let else_block = if self.eat(&T::Else).is_some() {
                    Some(self.block(annotation)?)
                } else {
                    None
                };
                StmtKind::If {
                    condition,
                    then_block,
                    else_block,
                }
            }
            T::While => {
                self.bump();
                let condition = self.condition()?;
                let body = self.block(annotation)?;
                StmtKind::While { condition, body }
            }
            T::Return => {
                self.bump();
                let value = self.expression(1)?.expr;
                self.expect(&T::Semicolon, "';' after return")?;
                StmtKind::Return(value)
            }
            T::Print => {
                self.bump();
                self.expect(&T::LParen, "'(' after print")?;
                let value = self.expression(1)?.expr;
                self.expect(&T::RParen, "')' after the print argument")?;
                self.expect(&T::Semicolon, "';' after print")?;
                StmtKind::Print(value)
            }
            T::Break | T::Continue => {
                let is_break = self.at(&T::Break);
                self.bump();
                self.expect(&T::Semicolon, "';' after break or continue")?;
                if is_break {
                    StmtKind::Break
                } else {
                    StmtKind::Continue
                }
            }
            T::LBrace => StmtKind::Block(self.block(annotation)?),
            T::Ident(_) if matches!(self.peek(1).map(|t| &t.kind), Some(T::Dot)) => {
                let (name, name_span) = self.identifier()?;
                self.bump();
                let (field, field_span) = self.field_path()?;
                self.expect(&T::Eq, "'=' after a record field assignment target")?;
                let value = self.expression(1)?.expr;
                self.expect(&T::Semicolon, "';' after field assignment")?;
                StmtKind::FieldAssign {
                    name,
                    name_span,
                    field,
                    field_span,
                    value,
                }
            }
            T::Ident(_) if matches!(self.peek(1).map(|t| &t.kind), Some(T::Eq)) => {
                let (name, name_span) = self.identifier()?;
                self.bump();
                let value = self.expression(1)?.expr;
                self.expect(&T::Semicolon, "';' after assignment")?;
                StmtKind::Assign {
                    name,
                    name_span,
                    value,
                }
            }
            T::Ident(_) if matches!(self.peek(1).map(|t| &t.kind), Some(T::LBracket)) => {
                let (name, name_span) = self.identifier()?;
                let target = self.indexed(name, name_span)?.expr;
                self.expect(&T::Eq, "'=' after an indexed assignment target")?;
                let value = self.expression(1)?.expr;
                self.expect(&T::Semicolon, "';' after indexed assignment")?;
                let ExprKind::Index { name, index } = target.kind else {
                    unreachable!("indexed creates an Index expression")
                };
                StmtKind::IndexAssign {
                    name,
                    name_span,
                    index: *index,
                    value,
                }
            }
            _ => {
                let value = self.expression(1)?.expr;
                if !matches!(
                    value.kind,
                    ExprKind::Call { .. }
                        | ExprKind::GenericCall { .. }
                        | ExprKind::Intrinsic { .. }
                        | ExprKind::TensorCall { .. }
                        | ExprKind::Try(_)
                ) {
                    self.error(
                        "E_PARSE_STATEMENT",
                        value.span,
                        "only function or intrinsic calls may be used as expression statements",
                    );
                    return Err(());
                }
                self.expect(&T::Semicolon, "';' after a function call")?;
                StmtKind::Expr(value)
            }
        };
        let end = self.last_span;
        Ok(Stmt {
            kind,
            span: start.join(end),
        })
    }
    fn expression(&mut self, min_precedence: u8) -> ParseResult<ParsedExpr> {
        self.enter()?;
        let result = self.expression_inner(min_precedence);
        self.nesting -= 1;
        result
    }
    fn condition(&mut self) -> ParseResult<Expr> {
        // A following brace starts the branch/loop, even if a value binding
        // shares its spelling with a record type. Constructors are initializers.
        let previous = self.record_literals;
        self.record_literals = false;
        let result = self.expression(1).map(|value| value.expr);
        self.record_literals = previous;
        result
    }
    fn expression_inner(&mut self, min_precedence: u8) -> ParseResult<ParsedExpr> {
        let mut left = self.primary()?;
        while let Some((op, precedence)) = binary(&self.current().kind) {
            if precedence < min_precedence {
                break;
            }
            self.bump();
            let right = self.expression(precedence + 1)?;
            let depth = left.depth.max(right.depth) + 1;
            let span = left.expr.span.join(right.expr.span);
            self.check_depth(depth, span)?;
            left = ParsedExpr {
                expr: Expr {
                    kind: ExprKind::Binary {
                        op,
                        left: Box::new(left.expr),
                        right: Box::new(right.expr),
                    },
                    span,
                },
                depth,
            };
        }
        Ok(left)
    }
    fn check_depth(&mut self, depth: usize, span: Span) -> ParseResult<()> {
        if depth + self.blocks > MAX_NESTING {
            self.error(
                "E_NESTING_LIMIT",
                span,
                format!("expression and block depth exceeds the limit of {MAX_NESTING}"),
            );
            Err(())
        } else {
            Ok(())
        }
    }
    fn indexed(&mut self, name: String, start: Span) -> ParseResult<ParsedExpr> {
        self.expect(&T::LBracket, "'[' before an index")?;
        let index = self.expression(1)?;
        let end = self.expect(&T::RBracket, "']' after an index")?.span;
        let span = start.join(end);
        let depth = index.depth + 1;
        self.check_depth(depth, span)?;
        Ok(ParsedExpr {
            expr: Expr {
                kind: ExprKind::Index {
                    name,
                    index: Box::new(index.expr),
                },
                span,
            },
            depth,
        })
    }
    fn primary(&mut self) -> ParseResult<ParsedExpr> {
        let cursor = self.mark();
        let token = self.bump();
        let span = token.span;
        let kind = match token.kind {
            T::Integer(digits) => match digits.parse::<i64>() {
                Ok(value) => ExprKind::Int(value),
                Err(_) => {
                    self.error(
                        "E_INTEGER_RANGE",
                        span,
                        "integer literal is outside the i64 range",
                    );
                    return Err(());
                }
            },
            T::Float(bits) => ExprKind::Float(bits),
            T::True => ExprKind::Bool(true),
            T::False => ExprKind::Bool(false),
            T::Ident(mut name) => {
                let mut type_args = None;
                let qualified = self.eat(&T::DoubleColon).is_some();
                if qualified {
                    if self.at(&T::Lt) {
                        type_args = Some(self.type_arguments()?);
                    } else {
                        let (mut member, mut member_span) = self.identifier()?;
                        if self.eat(&T::DoubleColon).is_some() {
                            if self.at(&T::Lt) {
                                type_args = Some(self.type_arguments()?);
                            } else {
                                name.push_str("::");
                                name.push_str(&member);
                                (member, member_span) = self.identifier()?;
                                if !(self.at(&T::LParen)
                                    || self.record_literals && self.at(&T::LBrace))
                                {
                                    self.expected(
                                        "'{' after a qualified union variant or '(' after a trait method",
                                    );
                                    return Err(());
                                }
                            }
                        }
                        if type_args.is_none() && self.record_literals && self.at(&T::LBrace) {
                            return self.union_constructor(name, member, member_span, span, None);
                        }
                        name.push_str("::");
                        name.push_str(&member);
                    }
                    if type_args.is_some() && self.record_literals && self.at(&T::DoubleColon) {
                        let application_span = span.join(self.last_span);
                        let application = self.nominal_application(
                            &name,
                            application_span,
                            type_args.take().unwrap(),
                        )?;
                        self.bump();
                        let (variant, variant_span) = self.identifier()?;
                        return self.union_constructor(
                            name,
                            variant,
                            variant_span,
                            span,
                            Some(application),
                        );
                    }
                    if !(self.at(&T::LParen)
                        || type_args.is_some() && self.record_literals && self.at(&T::LBrace))
                    {
                        self.expected("'(' after a qualified function name");
                        return Err(());
                    }
                }
                if self.eat(&T::LParen).is_some() {
                    let mut args = Vec::new();
                    let mut depth = 1;
                    if !self.at(&T::RParen) {
                        loop {
                            let arg = self.expression(1)?;
                            depth = depth.max(arg.depth + 1);
                            args.push(arg.expr);
                            if self.eat(&T::Comma).is_none() || self.at(&T::RParen) {
                                break;
                            }
                        }
                    }
                    let end = self
                        .expect(&T::RParen, "')' after function arguments")?
                        .span;
                    let span = span.join(end);
                    self.check_depth(depth, span)?;
                    return Ok(ParsedExpr {
                        expr: Expr {
                            kind: if let Some(type_args) = type_args {
                                ExprKind::GenericCall {
                                    name,
                                    type_args,
                                    args,
                                }
                            } else {
                                ExprKind::Call { name, args }
                            },
                            span,
                        },
                        depth,
                    });
                }
                if self.at(&T::LBracket) {
                    return self.indexed(name, span);
                }
                if self.eat(&T::Dot).is_some() {
                    let (field, field_span) = self.field_path()?;
                    return Ok(ParsedExpr {
                        expr: Expr {
                            kind: ExprKind::Field {
                                name,
                                field,
                                field_span,
                            },
                            span: span.join(field_span),
                        },
                        depth: 1,
                    });
                }
                if self.record_literals && self.eat(&T::LBrace).is_some() {
                    let application = type_args
                        .take()
                        .map(|args| {
                            self.nominal_application(&name, span.join(self.last_span), args)
                        })
                        .transpose()?;
                    let mut fields = Vec::new();
                    let mut depth = 1;
                    while !self.at(&T::RBrace) {
                        if fields.len() >= MAX_RECORD_FIELDS {
                            self.error(
                                "R002",
                                self.current().span,
                                format!("record construction exceeds {MAX_RECORD_FIELDS} fields"),
                            );
                            return Err(());
                        }
                        let (name, name_span) = self.identifier()?;
                        self.expect(&T::Colon, "':' before an explicit record field value")?;
                        let value = self.expression(1)?;
                        depth = depth.max(value.depth + 1);
                        self.check_depth(depth, span.join(value.expr.span))?;
                        fields.push(FieldInitializer {
                            name,
                            name_span,
                            value: value.expr,
                        });
                        if self.eat(&T::Comma).is_none() {
                            break;
                        }
                    }
                    let end = self
                        .expect(&T::RBrace, "'}' after record construction")?
                        .span;
                    return Ok(ParsedExpr {
                        expr: Expr {
                            kind: ExprKind::Record {
                                name,
                                fields,
                                application,
                            },
                            span: span.join(end),
                        },
                        depth,
                    });
                }
                ExprKind::Var(name)
            }
            T::LBracket => {
                let mut elements = Vec::new();
                let mut depth = 1;
                if !self.at(&T::RBracket) {
                    loop {
                        let element = self.expression(1)?;
                        depth = depth.max(element.depth + 1);
                        self.check_depth(depth, span.join(element.expr.span))?;
                        elements.push(element.expr);
                        if self.at(&T::Semicolon) {
                            self.error("E_PARSE_ARRAY_REPEAT", self.current().span,
                                "array repetition is unsupported; list every element separated by commas");
                            return Err(());
                        }
                        if self.eat(&T::Comma).is_none() || self.at(&T::RBracket) {
                            break;
                        }
                    }
                }
                let end = self.expect(&T::RBracket, "']' after array elements")?.span;
                let span = span.join(end);
                self.check_depth(depth, span)?;
                return Ok(ParsedExpr {
                    expr: Expr {
                        kind: ExprKind::Array(elements),
                        span,
                    },
                    depth,
                });
            }
            T::Amp => {
                let mutable = self.eat(&T::Mut).is_some();
                let (name, name_span) = self.identifier()?;
                let mut depth = 1;
                let (range, end) = if self.eat(&T::LBracket).is_some() {
                    let lo = self.expression(1)?;
                    self.expect(&T::DotDot, "'..' between explicit slice endpoints")?;
                    let hi = self.expression(1)?;
                    depth = 1 + lo.depth.max(hi.depth);
                    let end = self.expect(&T::RBracket, "']' after slice endpoints")?.span;
                    (Some((Box::new(lo.expr), Box::new(hi.expr))), end)
                } else {
                    (None, name_span)
                };
                let span = span.join(end);
                self.check_depth(depth, span)?;
                return Ok(ParsedExpr {
                    expr: Expr {
                        kind: ExprKind::Borrow {
                            name,
                            mutable,
                            range,
                        },
                        span,
                    },
                    depth,
                });
            }
            T::Len => {
                self.expect(&T::LParen, "'(' after len")?;
                let (name, _) = self.identifier()?;
                let end = self
                    .expect(&T::RParen, "')' after the array or slice name")?
                    .span;
                let span = span.join(end);
                self.check_depth(1, span)?;
                return Ok(ParsedExpr {
                    expr: Expr {
                        kind: ExprKind::Len(name),
                        span,
                    },
                    depth: 1,
                });
            }
            T::Try => {
                if !matches!(self.current().kind, T::TensorIntrinsic(op) if op.fallible()) {
                    self.error(
                        "E_PARSE_TRY",
                        self.current().span,
                        "try must directly precede one fallible tensor builtin call",
                    );
                    return Err(());
                }
                let value = self.primary()?;
                let span = span.join(value.expr.span);
                let depth = value.depth + 1;
                self.check_depth(depth, span)?;
                return Ok(ParsedExpr {
                    expr: Expr {
                        kind: ExprKind::Try(Box::new(value.expr)),
                        span,
                    },
                    depth,
                });
            }
            T::Intrinsic(_) | T::TensorIntrinsic(_) => {
                self.expect(&T::LParen, "'(' after the intrinsic name")?;
                let mut args = Vec::new();
                let mut depth = 1;
                if !self.at(&T::RParen) {
                    loop {
                        let arg = self.expression(1)?;
                        depth = depth.max(arg.depth + 1);
                        self.check_depth(depth, span.join(arg.expr.span))?;
                        args.push(arg.expr);
                        if self.eat(&T::Comma).is_none() || self.at(&T::RParen) {
                            break;
                        }
                    }
                }
                let end = self
                    .expect(&T::RParen, "')' after intrinsic arguments")?
                    .span;
                let span = span.join(end);
                self.check_depth(depth, span)?;
                return Ok(ParsedExpr {
                    expr: Expr {
                        kind: match token.kind {
                            T::Intrinsic(op) => ExprKind::Intrinsic { op, args },
                            T::TensorIntrinsic(op) => ExprKind::TensorCall { op, args },
                            _ => unreachable!("intrinsic token"),
                        },
                        span,
                    },
                    depth,
                });
            }
            T::Minus | T::Bang | T::Tilde => {
                let op = match token.kind {
                    T::Minus => UnaryOp::Neg,
                    T::Bang => UnaryOp::Not,
                    _ => UnaryOp::BitNot,
                };
                // i64::MIN has no representable positive i64 counterpart.
                if op == UnaryOp::Neg {
                    if let T::Integer(digits) = &self.current().kind {
                        if digits.parse::<u64>() == Ok(1u64 << 63) {
                            let end = self.bump().span;
                            return Ok(ParsedExpr {
                                expr: Expr {
                                    kind: ExprKind::Int(i64::MIN),
                                    span: span.join(end),
                                },
                                depth: 1,
                            });
                        }
                    }
                }
                let value = self.expression(11)?;
                let span = span.join(value.expr.span);
                let depth = value.depth + 1;
                self.check_depth(depth, span)?;
                return Ok(ParsedExpr {
                    expr: Expr {
                        kind: ExprKind::Unary {
                            op,
                            value: Box::new(value.expr),
                        },
                        span,
                    },
                    depth,
                });
            }
            T::LParen => {
                if let Some(end) = self.eat(&T::RParen) {
                    self.check_depth(1, span.join(end.span))?;
                    return Ok(ParsedExpr {
                        expr: Expr {
                            kind: ExprKind::Unit,
                            span: span.join(end.span),
                        },
                        depth: 1,
                    });
                }
                let mut value = self.expression(1)?;
                let end = self.expect(&T::RParen, "')' after the expression")?.span;
                value.expr.span = span.join(end);
                return Ok(value);
            }
            _ => {
                // Leave structural delimiters for block and statement recovery.
                if !matches!(token.kind, T::Eof) {
                    self.restore(cursor);
                }
                self.error(
                    "E_PARSE_EXPRESSION",
                    span,
                    "expected a literal, variable, call, borrow, index, len, or parenthesized expression",
                );
                return Err(());
            }
        };
        self.check_depth(1, span)?;
        Ok(ParsedExpr {
            expr: Expr { kind, span },
            depth: 1,
        })
    }
}

// Increasing binding strength. Every binary operator associates to the left.
fn binary(kind: &T) -> Option<(BinaryOp, u8)> {
    Some(match kind {
        T::OrOr => (BinaryOp::Or, 1),
        T::AndAnd => (BinaryOp::And, 2),
        T::Pipe => (BinaryOp::BitOr, 3),
        T::Caret => (BinaryOp::BitXor, 4),
        T::Amp => (BinaryOp::BitAnd, 5),
        T::EqEq => (BinaryOp::Eq, 6),
        T::Ne => (BinaryOp::Ne, 6),
        T::Lt => (BinaryOp::Lt, 7),
        T::Le => (BinaryOp::Le, 7),
        T::Gt => (BinaryOp::Gt, 7),
        T::Ge => (BinaryOp::Ge, 7),
        T::Shl => (BinaryOp::Shl, 8),
        T::Shr => (BinaryOp::Shr, 8),
        T::Plus => (BinaryOp::Add, 9),
        T::Minus => (BinaryOp::Sub, 9),
        T::Star => (BinaryOp::Mul, 10),
        T::Slash => (BinaryOp::Div, 10),
        T::Percent => (BinaryOp::Rem, 10),
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn returning(expression: &str) -> Program {
        parse(&format!("fn main() -> i64 {{ return {expression}; }}")).unwrap()
    }
    fn value(program: &Program) -> &Expr {
        match &program.functions[0].body.stmts[0].kind {
            StmtKind::Return(value) => value,
            _ => panic!("expected return"),
        }
    }
    fn shape(expr: &Expr) -> String {
        match &expr.kind {
            ExprKind::Int(n) => n.to_string(),
            ExprKind::Float(bits) => format!("{:?}", f64::from_bits(*bits)),
            ExprKind::Bool(b) => b.to_string(),
            ExprKind::Unit => "()".into(),
            ExprKind::Var(v) => v.clone(),
            ExprKind::Field { name, field, .. } => format!("{name}.{field}"),
            ExprKind::Record { name, fields, .. } => format!(
                "{name}{{{}}}",
                fields
                    .iter()
                    .map(|field| format!("{}:{}", field.name, shape(&field.value)))
                    .collect::<Vec<_>>()
                    .join(",")
            ),
            ExprKind::Union {
                name,
                variant,
                fields,
                ..
            } => format!(
                "{name}::{variant}{{{}}}",
                fields
                    .iter()
                    .map(|field| format!("{}:{}", field.name, shape(&field.value)))
                    .collect::<Vec<_>>()
                    .join(",")
            ),
            ExprKind::Array(elements) => format!(
                "[{}]",
                elements.iter().map(shape).collect::<Vec<_>>().join(",")
            ),
            ExprKind::Borrow {
                name,
                mutable,
                range,
            } => format!(
                "&{}{}{}",
                if *mutable { "mut " } else { "" },
                name,
                range.as_ref().map_or(String::new(), |(lo, hi)| format!(
                    "[{}..{}]",
                    shape(lo),
                    shape(hi)
                ))
            ),
            ExprKind::Index { name, index } => format!("{name}[{}]", shape(index)),
            ExprKind::Len(name) => format!("len({name})"),
            ExprKind::Unary { op, value } => format!("({op:?} {})", shape(value)),
            ExprKind::Binary { op, left, right } => {
                format!("({op:?} {} {})", shape(left), shape(right))
            }
            ExprKind::Call { name, args } => format!(
                "{}({})",
                name,
                args.iter().map(shape).collect::<Vec<_>>().join(",")
            ),
            ExprKind::GenericCall {
                name,
                type_args,
                args,
            } => format!(
                "{name}::<{type_args:?}>({})",
                args.iter().map(shape).collect::<Vec<_>>().join(",")
            ),
            ExprKind::Intrinsic { op, args } => format!(
                "{op:?}({})",
                args.iter().map(shape).collect::<Vec<_>>().join(",")
            ),
            ExprKind::TensorCall { op, args } => format!(
                "{}({})",
                op.name(),
                args.iter().map(shape).collect::<Vec<_>>().join(",")
            ),
            ExprKind::Try(value) => format!("(try {})", shape(value)),
        }
    }

    #[test]
    fn generic_headers_discard_syntax_placeholders_before_resolving_body_types() {
        let source = "import data; export fn forward<T>(x:T,packet:data::Packet)->T{let next:data::Packet=identity::<data::Packet>(packet);return x;}";
        let tokens = lex(source).unwrap();
        let headers = parse_headers(&tokens).unwrap();
        assert!(headers.exported);
        assert_eq!(headers.functions, 0);
        assert_eq!(headers.generic_functions, 1);
        assert_eq!(headers.imports[0].name, "data");
        assert!(parse_tokens(tokens.clone())
            .unwrap_err()
            .iter()
            .any(|error| error.code == "E_PARSE_TYPE"));
        let id = RecordId {
            source: crate::diagnostic::SourceId(7),
            offset: 123,
        };
        let parsed = parse_tokens_in_scope(
            tokens,
            &HashMap::from([(
                "data::Packet".into(),
                NominalHeader {
                    id,
                    kind: NominalKind::Record,
                    arity: 0,
                    name_span: Span::in_source(id.source, id.offset, id.offset + 6),
                },
            )]),
        )
        .unwrap();
        let template = &parsed.generic_functions[0];
        assert_eq!(
            template.function.params[1].ty,
            TypeExpr::Concrete(Type::Record(id.into()))
        );
        let StmtKind::Let { ty, value, .. } = &template.function.body.stmts[0].kind else {
            panic!("generic body binding")
        };
        assert_eq!(*ty, TypeExpr::Concrete(Type::Record(id.into())));
        let ExprKind::GenericCall { type_args, .. } = &value.kind else {
            panic!("generic call")
        };
        assert_eq!(type_args, &[TypeExpr::Concrete(Type::Record(id.into()))]);
    }

    #[test]
    fn records_resolve_forward_types_by_source_identity_and_preserve_field_order() {
        let source = "fn main()->i64{let r:Pair=Pair{right:2,left:1};return r.left;} record Pair{left:i64,right:i64}";
        let source_id = crate::diagnostic::SourceId(17);
        let parsed = parse_tokens(crate::lexer::lex_in_source(source, source_id).unwrap()).unwrap();
        let record = &parsed.records[0];
        assert_eq!(record.id.declaration().unwrap().source, source_id);
        assert_eq!(
            record.id.declaration().unwrap().offset,
            source.rfind("Pair").unwrap()
        );
        let StmtKind::Let { ty, value, .. } = &parsed.functions[0].body.stmts[0].kind else {
            panic!("record let")
        };
        assert_eq!(*ty, Type::Record(record.id));
        assert_eq!(shape(value), "Pair{right:2,left:1}");
        let StmtKind::Return(value) = &parsed.functions[0].body.stmts[1].kind else {
            panic!("field return")
        };
        assert_eq!(shape(value), "r.left");
        assert_eq!(value.span.source, source_id);
    }

    #[test]
    fn precedence_associativity_and_unary() {
        assert_eq!(
            shape(value(&returning("1 + 2 * 3 - 4 / 2"))),
            "(Sub (Add 1 (Mul 2 3)) (Div 4 2))"
        );
        assert_eq!(shape(value(&returning("8 - 3 - 1"))), "(Sub (Sub 8 3) 1)");
        assert_eq!(
            shape(value(&returning("-2 * ~3"))),
            "(Mul (Neg 2) (BitNot 3))"
        );
        assert_eq!(shape(value(&returning("a || b && c | d ^ e & f == g < h << i + j * k"))),
            "(Or a (And b (BitOr c (BitXor d (BitAnd e (Eq f (Lt g (Shl h (Add i (Mul j k))))))))))");
        assert_eq!(shape(value(&returning("f(1, 2 + 3)"))), "f(1,(Add 2 3))");
    }

    #[test]
    fn parses_all_statement_forms() {
        let program = parse(
            "fn f(x: i64, flag: bool,) -> i64 {\n\
            let mut n: i64 = x; let ok: bool = !flag; n = n + 1; print(n);\n\
            if ok { n = 2; } else { n = 3; }\n\
            while n < 9 { n = n + 1; if n == 5 { continue; } break; }\n\
            { f(n, true,); } return n; }\n\
            fn main() -> i64 { return f(0, false); }",
        )
        .unwrap();
        assert_eq!(program.functions.len(), 2);
        assert_eq!(program.functions[0].params.len(), 2);
        assert_eq!(program.functions[0].body.stmts.len(), 8);
        assert!(matches!(
            program.functions[0].body.stmts[0].kind,
            StmtKind::Let { mutable: true, .. }
        ));
    }

    #[test]
    fn parses_array_slice_types_and_indexed_assignment() {
        let source = "fn f(a: [i64;2], s: &[i64], t: &mut[i64]) -> [i64;2] {\n\
            let mut data: [i64;2] = [1, 2,];\n\
            let shared: &[i64] = &data;\n\
            let unique: &mut[i64] = &mut data[0..len(data)];\n\
            data[len(data)-1] = s[0] + 1; return data; }\n\
            fn g() -> &mut[i64] { return &mut data; }";
        let program = parse(source).unwrap();
        let f = &program.functions[0];
        assert_eq!(
            f.params.iter().map(|p| p.ty).collect::<Vec<_>>(),
            vec![
                Type::Array {
                    element: ElementType::I64,
                    length: 2
                },
                Type::Slice {
                    element: ElementType::I64,
                    mutable: false
                },
                Type::Slice {
                    element: ElementType::I64,
                    mutable: true
                }
            ]
        );
        assert_eq!(
            f.return_type,
            Type::Array {
                element: ElementType::I64,
                length: 2
            }
        );
        assert_eq!(
            program.functions[1].return_type,
            Type::Slice {
                element: ElementType::I64,
                mutable: true
            }
        );
        match &f.body.stmts[3].kind {
            StmtKind::IndexAssign {
                name,
                name_span,
                index,
                value,
            } => {
                assert_eq!(name, "data");
                assert_eq!(&source[name_span.start..name_span.end], "data");
                assert_eq!(shape(index), "(Sub len(data) 1)");
                assert_eq!(shape(value), "(Add s[0] 1)");
            }
            _ => panic!("expected indexed assignment"),
        }
        for length in [0, 1, 32, 33, usize::MAX] {
            let p = parse(&format!("fn f(a: [i64;{length}]) -> i64 {{ return 0; }}")).unwrap();
            assert_eq!(
                p.functions[0].params[0].ty,
                Type::Array {
                    element: ElementType::I64,
                    length
                }
            );
        }
    }

    #[test]
    fn parses_f64_scalars_and_homogeneous_aggregate_type_annotations() {
        let source = "fn calculate(x:f64, a:[f64;2], s:&[f64], t:&mut[f64]) -> f64 {\n\
            let mut data:[f64;2] = [0.0, 1e2];\n\
            let view:&[f64] = &data[0..2];\n\
            let edit:&mut[f64] = &mut data; data[0] = x / 2.0; return x; }";
        let program = parse(source).unwrap();
        let function = &program.functions[0];
        assert_eq!(
            function.params.iter().map(|p| p.ty).collect::<Vec<_>>(),
            vec![
                Type::F64,
                Type::Array {
                    element: ElementType::F64,
                    length: 2
                },
                Type::Slice {
                    element: ElementType::F64,
                    mutable: false
                },
                Type::Slice {
                    element: ElementType::F64,
                    mutable: true
                },
            ]
        );
        assert_eq!(function.return_type, Type::F64);
        assert_eq!(function.params[1].ty.to_string(), "[f64; 2]");
        assert_eq!(function.params[2].ty.to_string(), "&[f64]");
        assert_eq!(function.params[3].ty.to_string(), "&mut [f64]");
        assert_eq!(ElementType::F64.scalar_type(), Type::F64);
        assert_eq!(ElementType::I64.scalar_type(), Type::I64);
        for invalid_type in [
            "[bool;2]",
            "[f64;2.0]",
            "[f64;2e0]",
            "&[bool]",
            "[[f64;2];2]",
        ] {
            assert!(parse(&format!("fn f(a:{invalid_type})->i64 {{ return 0; }}")).is_err());
        }
        // Literal homogeneity and explicit scalar conversions are semantic checks.
        assert!(parse("fn f()->f64 { let a:[f64;2] = [1,2.0]; return 1; }").is_ok());
    }

    #[test]
    fn float_literals_preserve_bits_unary_minus_and_whole_source_spans() {
        let source = "// 雪\nfn f()->f64 { return -0.0 + 0.1 * 2E-3; }";
        let program = parse(source).unwrap();
        let expr = value(&program);
        assert_eq!(&source[expr.span.start..expr.span.end], "-0.0 + 0.1 * 2E-3");
        assert_eq!(shape(expr), "(Add (Neg 0.0) (Mul 0.1 0.002))");
        let minus_zero = returning("-0.0");
        assert!(
            matches!(&value(&minus_zero).kind, ExprKind::Unary { op: UnaryOp::Neg, value }
            if matches!(value.kind, ExprKind::Float(0)))
        );
        assert!(matches!(
            value(&returning("1e-9999")).kind,
            ExprKind::Float(0)
        ));
        assert!(matches!(value(&returning("1")).kind, ExprKind::Int(1)));
    }

    #[test]
    fn intrinsic_calls_keep_arity_for_checking_and_reject_name_collisions() {
        for (expression, expected) in [
            ("f64_from_i64(7)", "FromI64(7)"),
            ("f64_bits(-0.0)", "ToBits((Neg 0.0))"),
            ("f64_from_bits(-1)", "FromBits((Neg 1))"),
            ("f64_bits(f64_from_i64(7))", "ToBits(FromI64(7))"),
            ("f64_bits()", "ToBits()"),
            ("f64_bits(1.0,2.0,)", "ToBits(1.0,2.0)"),
        ] {
            let source = format!("fn f()->i64 {{ return {expression}; }}");
            let program = parse(&source).unwrap();
            let expr = value(&program);
            assert_eq!(shape(expr), expected);
            assert_eq!(&source[expr.span.start..expr.span.end], expression);
        }
        assert!(parse("fn f()->i64 { f64_bits(1.0); return 0; }").is_ok());
        for reserved in ["f64", "f64_from_i64", "f64_bits", "f64_from_bits"] {
            for source in [
                format!("fn {reserved}()->i64 {{ return 0; }}"),
                format!("fn f({reserved}:i64)->i64 {{ return 0; }}"),
                format!("fn f()->i64 {{ let {reserved}:i64=0; return 0; }}"),
            ] {
                assert!(parse(&source).is_err(), "{source}");
            }
        }
        let errors = parse(
            "fn f()->i64 { let x:f64=f64_from_i64(; let y:i64=f64_bits(); print(); return 0; }",
        )
        .unwrap_err();
        assert_eq!(errors.len(), 2);
    }

    #[test]
    fn intrinsic_nesting_obeys_existing_bounds() {
        let accepted = format!(
            "{}0.0{}",
            "f64_bits(".repeat(MAX_NESTING - 2),
            ")".repeat(MAX_NESTING - 2)
        );
        assert!(parse(&format!("fn f()->i64 {{ return {accepted}; }}")).is_ok());
        for count in [MAX_NESTING - 1, 2000] {
            let rejected = format!("{}0.0{}", "f64_bits(".repeat(count), ")".repeat(count));
            assert!(parse(&format!("fn f()->i64 {{ return {rejected}; }}"))
                .unwrap_err()
                .iter()
                .any(|error| error.code == "E_NESTING_LIMIT"));
        }
    }

    #[test]
    fn aggregate_expression_shapes_precedence_and_spans() {
        for (expression, expected) in [
            ("[]", "[]"),
            ("[1, f(), a[0],]", "[1,f(),a[0]]"),
            ("[[1], [2]]", "[[1],[2]]"),
            ("&a", "&a"),
            ("&mut a", "&mut a"),
            ("&a[0..2]", "&a[0..2]"),
            ("&mut a[i+1..len(a)-1]", "&mut a[(Add i 1)..(Sub len(a) 1)]"),
            ("x & a[i]", "(BitAnd x a[i])"),
            ("f(&mut a[0..2])", "f(&mut a[0..2])"),
            ("-a[i]*len(a)", "(Mul (Neg a[i]) len(a))"),
        ] {
            let source = format!("// λ\nfn main() -> i64 {{ return {expression}; }}");
            let program = parse(&source).unwrap();
            let expr = value(&program);
            assert_eq!(shape(expr), expected);
            assert_eq!(&source[expr.span.start..expr.span.end], expression);
        }
    }

    #[test]
    fn rejects_unsupported_aggregate_syntax_and_reserved_len_names() {
        for expression in [
            "[,]",
            "[1,,2]",
            "[1;3]",
            "&a[..]",
            "&a[..2]",
            "&a[1..]",
            "&a[1]",
            "&a[0..=2]",
            "(a)[0]",
            "f()[0]",
            "[1][0]",
            "a[0][1]",
            "&(a)",
            "&f()",
            "len()",
            "len(a,b)",
            "len(a[0])",
            "len((a))",
            "a[]",
            "a[0..2]",
        ] {
            assert!(
                parse(&format!("fn f() -> i64 {{ return {expression}; }}")).is_err(),
                "{expression}"
            );
        }
        for ty in [
            "[bool;2]",
            "[i64;-1]",
            "[i64;1+1]",
            "&[bool]",
            "&mut[i64;2]",
            "[[i64;2];2]",
            "&&[i64]",
        ] {
            assert!(
                parse(&format!("fn f(a: {ty}) -> i64 {{ return 0; }}")).is_err(),
                "{ty}"
            );
        }
        let errors =
            parse("fn f(a: [i64;999999999999999999999999999]) -> i64 { return 0; }").unwrap_err();
        assert_eq!(errors[0].code, "E_ARRAY_LENGTH_RANGE");
        let errors = parse("fn f() -> i64 { return [1;3]; }").unwrap_err();
        assert_eq!(errors[0].code, "E_PARSE_ARRAY_REPEAT");
        for source in [
            "fn len() -> i64 { return 0; }",
            "fn f(len:i64) -> i64 { return 0; }",
            "fn f() -> i64 { let len:i64 = 0; return 0; }",
            "fn f() -> i64 { len(a); return 0; }",
        ] {
            assert!(parse(source).is_err(), "{source}");
        }
        for statement in ["a[0];", "a[0] += 1;", "a[0] = b[0] = 1;", "(a[0]) = 1;"] {
            assert!(
                parse(&format!("fn f() -> i64 {{ {statement} return 0; }}")).is_err(),
                "{statement}"
            );
        }
    }

    #[test]
    fn aggregate_errors_recover_and_truncated_inputs_terminate() {
        let source = "fn f() -> i64 { let a:[i64;2] = [1,,2]; let s:&[i64] = &a[0..]; a[] = 1; print(); return 0; } fn next() -> i64 { print(); return 0; }";
        let errors = parse(source).unwrap_err();
        assert_eq!(errors.len(), 5);
        assert!(errors
            .windows(2)
            .all(|pair| pair[0].span.start < pair[1].span.start));
        assert!(errors.last().unwrap().span.start > source.find("fn next").unwrap());
        for source in [
            "fn f(a:[i64;",
            "fn f() -> i64 { return [",
            "fn f() -> i64 { return a[",
            "fn f() -> i64 { return &a[0..",
            "fn f() -> i64 { return len(",
        ] {
            assert!(parse(source).is_err(), "{source}");
        }
    }

    #[test]
    fn aggregate_depth_limits_include_elements_indices_and_endpoints() {
        for (open, close) in [("[", "]"), ("a[", "]"), ("&a[", "..0]")] {
            let accepted = format!(
                "{}0{}",
                open.repeat(MAX_NESTING - 2),
                close.repeat(MAX_NESTING - 2)
            );
            assert!(
                parse(&format!("fn f() -> i64 {{ return {accepted}; }}")).is_ok(),
                "{open}"
            );
            for count in [MAX_NESTING - 1, 2000] {
                let rejected = format!("{}0{}", open.repeat(count), close.repeat(count));
                assert!(
                    parse(&format!("fn f() -> i64 {{ return {rejected}; }}"))
                        .unwrap_err()
                        .iter()
                        .any(|e| e.code == "E_NESTING_LIMIT"),
                    "{open}"
                );
            }
        }
        // A wide array remains shallow; total tokens still cap its allocation.
        let count = (crate::lexer::MAX_TOKENS - 12) / 2;
        let source = format!("fn f() -> i64 {{ return [{}]; }}", "0,".repeat(count));
        let program = parse(&source).unwrap();
        assert!(
            matches!(&value(&program).kind, ExprKind::Array(elements) if elements.len() == count)
        );
        let source = format!("fn f() -> i64 {{ return [{}]; }}", "0,".repeat(count + 1));
        assert_eq!(parse(&source).unwrap_err()[0].code, "E_TOKEN_LIMIT");
    }

    #[test]
    fn literal_limits_include_minimum() {
        assert!(matches!(
            value(&returning("9223372036854775807")).kind,
            ExprKind::Int(i64::MAX)
        ));
        assert!(matches!(
            value(&returning("-9223372036854775808")).kind,
            ExprKind::Int(i64::MIN)
        ));
        assert!(matches!(
            value(&returning("-0009223372036854775808")).kind,
            ExprKind::Int(i64::MIN)
        ));
        for expression in [
            "9223372036854775808",
            "-9223372036854775809",
            "99999999999999999999999999999",
        ] {
            let errors =
                parse(&format!("fn main() -> i64 {{ return {expression}; }}")).unwrap_err();
            assert_eq!(errors[0].code, "E_INTEGER_RANGE", "{expression}");
        }
    }

    #[test]
    fn spans_cover_original_utf8_source() {
        let source =
            "// λ\nfn main() -> i64 { let x: i64 = (1 + 2); return -9223372036854775808; }";
        let program = parse(source).unwrap();
        let function = &program.functions[0];
        assert_eq!(
            &source[function.name_span.start..function.name_span.end],
            "main"
        );
        assert_eq!(
            &source[function.span.start..function.span.end],
            &source[source.find("fn").unwrap()..]
        );
        match &function.body.stmts[0].kind {
            StmtKind::Let {
                value, name_span, ..
            } => {
                assert_eq!(&source[value.span.start..value.span.end], "(1 + 2)");
                assert_eq!(&source[name_span.start..name_span.end], "x");
            }
            _ => panic!("expected declaration"),
        }
        if let StmtKind::Return(value) = &function.body.stmts[1].kind {
            assert_eq!(
                &source[value.span.start..value.span.end],
                "-9223372036854775808"
            );
        } else {
            panic!("expected return");
        }
    }

    #[test]
    fn recovers_independent_statement_and_function_errors() {
        let source = "fn broken() -> i64 { let x: i64 = ; print(); return 0; } fn bad(x: str) -> i64 {} fn next() -> i64 { let y: i64 = ; return 0; }";
        let errors = parse(source).unwrap_err();
        assert_eq!(
            errors.iter().map(|e| e.code).collect::<Vec<_>>(),
            vec![
                "E_PARSE_EXPRESSION",
                "E_PARSE_EXPRESSION",
                "E_PARSE_TYPE",
                "E_PARSE_EXPRESSION"
            ]
        );
        assert!(errors.windows(2).all(|e| e[0].span.start < e[1].span.start));
        let errors = parse("fn a() -> i64 { return 1 fn b() -> i64 { print(); }").unwrap_err();
        assert!(errors.iter().any(|e| e.code == "E_PARSE_EXPRESSION"));
    }

    #[test]
    fn unsupported_syntax_has_diagnostics() {
        for source in [
            "struct S {}",
            "fn f() {}",
            "fn f() -> str {}",
            "fn f() -> i64 { 1 + 2; }",
            "fn f() -> i64 { let x = 1; }",
        ] {
            assert!(parse(source).is_err(), "{source}");
        }
    }

    #[test]
    fn rejects_deep_syntax_and_long_expression_trees_without_overflow() {
        let expressions = [
            format!("{}0{}", "(".repeat(2000), ")".repeat(2000)),
            format!("{}0", "!".repeat(2000)),
            "1+".repeat(2000) + "1",
            format!("{}0{}", "f(".repeat(2000), ")".repeat(2000)),
        ];
        for expression in expressions {
            let errors =
                parse(&format!("fn main() -> i64 {{ return {expression}; }}")).unwrap_err();
            assert!(errors.iter().any(|e| e.code == "E_NESTING_LIMIT"));
        }
        let source = format!(
            "fn main() -> i64 {{ {} return 0; {} }}",
            "{".repeat(2000),
            "}".repeat(2000)
        );
        assert!(parse(&source)
            .unwrap_err()
            .iter()
            .any(|e| e.code == "E_NESTING_LIMIT"));
    }

    #[test]
    fn accepts_exact_depth_boundaries_and_rejects_one_more() {
        for accepted in [
            "1+".repeat(MAX_NESTING - 2) + "1",
            "!".repeat(MAX_NESTING - 2) + "true",
            format!(
                "{}0{}",
                "(".repeat(MAX_NESTING - 2),
                ")".repeat(MAX_NESTING - 2)
            ),
        ] {
            assert!(parse(&format!("fn f() -> i64 {{ return {accepted}; }}")).is_ok());
        }
        for rejected in [
            "1+".repeat(MAX_NESTING - 1) + "1",
            "!".repeat(MAX_NESTING - 1) + "true",
            format!(
                "{}0{}",
                "(".repeat(MAX_NESTING - 1),
                ")".repeat(MAX_NESTING - 1)
            ),
        ] {
            assert!(parse(&format!("fn f() -> i64 {{ return {rejected}; }}"))
                .unwrap_err()
                .iter()
                .any(|e| e.code == "E_NESTING_LIMIT"));
        }
        assert!(parse(&format!(
            "fn f() -> i64 {{ {}{} }}",
            "{".repeat(MAX_NESTING - 1),
            "}".repeat(MAX_NESTING - 1)
        ))
        .is_ok());
        assert!(parse(&format!(
            "fn f() -> i64 {{ {}{} }}",
            "{".repeat(MAX_NESTING),
            "}".repeat(MAX_NESTING)
        ))
        .is_err());
    }

    #[test]
    fn malformed_token_corpus_terminates_with_valid_spans() {
        let pieces = [
            "fn", "f", "i64", "bool", "let", "mut", "print", "return", "if", "else", "while",
            "break", "continue", "(", ")", "{", "}", ":", ",", ";", "->", "=", "+", "!", "0",
            "true", "&&", "<", "[", "]", "..", "&", "len", "f64", "0.0", "1e-2", "f64_bits",
        ];
        let mut state = 0x5248595a45u64;
        for _ in 0..512 {
            let mut source = String::from("fn f() -> i64 { ");
            for _ in 0..80 {
                state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
                source.push_str(pieces[(state >> 32) as usize % pieces.len()]);
                source.push(' ');
            }
            source.push_str("} fn next() -> i64 { return 0; }");
            if let Err(errors) = parse(&source) {
                assert!(!errors.is_empty() && errors.len() <= MAX_DIAGNOSTICS);
                for error in errors {
                    assert!(error.span.start <= error.span.end && error.span.end <= source.len());
                    assert!(
                        source.is_char_boundary(error.span.start)
                            && source.is_char_boundary(error.span.end)
                    );
                }
            }
        }
    }

    #[test]
    fn limits_functions_and_diagnostics_and_terminates_on_delimiters() {
        assert!(parse(&"fn f() -> i64 { return 0; }".repeat(MAX_FUNCTIONS)).is_ok());
        let source = "fn f() -> i64 { return 0; }".repeat(MAX_FUNCTIONS + 1);
        assert_eq!(
            parse(&source).unwrap_err().last().unwrap().code,
            "E_FUNCTION_LIMIT"
        );
        let source = format!("fn f() -> i64 {{ {} }}", "print();".repeat(1000));
        assert_eq!(parse(&source).unwrap_err().len(), MAX_DIAGNOSTICS);
        for fragment in ["}", ")", "else", "{", "fn", "return", "let", ";"] {
            let source = format!("fn f() -> i64 {{ {} }}", fragment.repeat(1000));
            assert!(parse(&source).is_err());
        }
    }
}
