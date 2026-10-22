//! Consume bounded parsed syntax while resolving ordinary declaration annotations.
//!
//! This is a representation boundary, not a type or ownership checker. Generic
//! function templates and expression type arguments stay symbolic and still need
//! their existing opaque proof and concrete specialization. Spelling, provenance,
//! expression order, control flow and every unannotated expression are preserved.
use super::*;

impl SourceProgram {
    /// The current subset has no symbolic ordinary declaration annotations.
    /// Project loading crosses this boundary only after parsing strict syntax;
    /// header discovery can inspect source declarations without converting them.
    pub(crate) fn resolve_ordinary_annotations(
        self,
    ) -> Result<Program, Vec<crate::diagnostic::Diagnostic>> {
        if !self.generic_records.is_empty()
            || !self.generic_unions.is_empty()
            || !self.applications.is_empty()
        {
            return Err(vec![crate::diagnostic::Diagnostic::new(
                "G007",
                Span::default(),
                "generic nominal source requires the source preparation entry point",
            )]);
        }
        self.try_map_annotations(|ty, span| {
            ty.concrete().ok_or_else(|| {
                crate::diagnostic::Diagnostic::new(
                    "G007",
                    span,
                    "ordinary source annotation requires a resolved concrete type",
                )
            })
        })
        .map_err(|diagnostic| vec![diagnostic])
    }
}

impl<A> Program<A> {
    pub(crate) fn try_map_annotations<B, E>(
        self,
        mut resolve: impl FnMut(A, Span) -> Result<B, E>,
    ) -> Result<Program<B>, E> {
        let records = self
            .records
            .into_iter()
            .map(|record| {
                Ok(Record {
                    exported: record.exported,
                    id: record.id,
                    name: record.name,
                    name_span: record.name_span,
                    fields: fields(record.fields, &mut resolve)?,
                })
            })
            .collect::<Result<_, E>>()?;
        let unions = self
            .unions
            .into_iter()
            .map(|union| {
                Ok(Union {
                    exported: union.exported,
                    id: union.id,
                    name: union.name,
                    name_span: union.name_span,
                    variants: union
                        .variants
                        .into_iter()
                        .map(|variant| {
                            Ok(UnionVariant {
                                name: variant.name,
                                name_span: variant.name_span,
                                fields: fields(variant.fields, &mut resolve)?,
                            })
                        })
                        .collect::<Result<_, E>>()?,
                })
            })
            .collect::<Result<_, E>>()?;
        let traits = self
            .traits
            .into_iter()
            .map(|contract| {
                Ok(Trait {
                    exported: contract.exported,
                    id: contract.id,
                    name: contract.name,
                    name_span: contract.name_span,
                    methods: contract
                        .methods
                        .into_iter()
                        .map(|method| {
                            Ok(TraitMethod {
                                name: method.name,
                                name_span: method.name_span,
                                params: method
                                    .params
                                    .into_iter()
                                    .map(|param| {
                                        Ok(TraitParam {
                                            name: param.name,
                                            name_span: param.name_span,
                                            ty: trait_type(
                                                param.ty,
                                                param.name_span,
                                                &mut resolve,
                                            )?,
                                        })
                                    })
                                    .collect::<Result<_, E>>()?,
                                return_type: trait_type(
                                    method.return_type,
                                    method.name_span,
                                    &mut resolve,
                                )?,
                            })
                        })
                        .collect::<Result<_, E>>()?,
                })
            })
            .collect::<Result<_, E>>()?;
        let implementations = self
            .implementations
            .into_iter()
            .map(|implementation| {
                Ok(Implementation {
                    trait_name: implementation.trait_name,
                    trait_span: implementation.trait_span,
                    target: resolve(implementation.target, implementation.trait_span)?,
                    bindings: implementation.bindings,
                })
            })
            .collect::<Result<_, E>>()?;
        let functions = self
            .functions
            .into_iter()
            .map(|function| {
                Ok(Function {
                    exported: function.exported,
                    name: function.name,
                    name_span: function.name_span,
                    params: function
                        .params
                        .into_iter()
                        .map(|param| {
                            Ok(Param {
                                name: param.name,
                                name_span: param.name_span,
                                ty: resolve(param.ty, param.name_span)?,
                            })
                        })
                        .collect::<Result<_, E>>()?,
                    return_type: resolve(function.return_type, function.name_span)?,
                    body: block(function.body, &mut resolve)?,
                    span: function.span,
                })
            })
            .collect::<Result<_, E>>()?;
        Ok(Program {
            applications: self.applications,
            imports: self.imports,
            records,
            unions,
            traits,
            implementations,
            functions,
            generic_functions: self.generic_functions,
            generic_records: self.generic_records,
            generic_unions: self.generic_unions,
        })
    }
}

fn fields<A, B, E>(
    fields: Vec<RecordField<A>>,
    resolve: &mut impl FnMut(A, Span) -> Result<B, E>,
) -> Result<Vec<RecordField<B>>, E> {
    fields
        .into_iter()
        .map(|field| {
            Ok(RecordField {
                name: field.name,
                name_span: field.name_span,
                ty: resolve(field.ty, field.name_span)?,
            })
        })
        .collect()
}

fn trait_type<A, B, E>(
    ty: TraitType<A>,
    span: Span,
    resolve: &mut impl FnMut(A, Span) -> Result<B, E>,
) -> Result<TraitType<B>, E> {
    match ty {
        TraitType::Concrete(ty) => resolve(ty, span).map(TraitType::Concrete),
        TraitType::SelfType => Ok(TraitType::SelfType),
    }
}

fn block<A, B, E>(
    block: Block<A>,
    resolve: &mut impl FnMut(A, Span) -> Result<B, E>,
) -> Result<Block<B>, E> {
    Ok(Block {
        span: block.span,
        stmts: block
            .stmts
            .into_iter()
            .map(|stmt| {
                let kind = match stmt.kind {
                    StmtKind::Let {
                        name,
                        name_span,
                        mutable,
                        ty,
                        value,
                    } => StmtKind::Let {
                        name,
                        name_span,
                        mutable,
                        ty: resolve(ty, name_span)?,
                        value,
                    },
                    StmtKind::Assign {
                        name,
                        name_span,
                        value,
                    } => StmtKind::Assign {
                        name,
                        name_span,
                        value,
                    },
                    StmtKind::IndexAssign {
                        name,
                        name_span,
                        index,
                        value,
                    } => StmtKind::IndexAssign {
                        name,
                        name_span,
                        index,
                        value,
                    },
                    StmtKind::FieldAssign {
                        name,
                        name_span,
                        field,
                        field_span,
                        value,
                    } => StmtKind::FieldAssign {
                        name,
                        name_span,
                        field,
                        field_span,
                        value,
                    },
                    StmtKind::Match {
                        name,
                        name_span,
                        arms,
                    } => StmtKind::Match {
                        name,
                        name_span,
                        arms: arms
                            .into_iter()
                            .map(|arm| {
                                Ok(MatchArm {
                                    application: arm.application,
                                    union_name: arm.union_name,
                                    union_span: arm.union_span,
                                    variant: arm.variant,
                                    variant_span: arm.variant_span,
                                    fields: arm.fields,
                                    body: self::block(arm.body, resolve)?,
                                })
                            })
                            .collect::<Result<_, E>>()?,
                    },
                    StmtKind::If {
                        condition,
                        then_block,
                        else_block,
                    } => StmtKind::If {
                        condition,
                        then_block: self::block(then_block, resolve)?,
                        else_block: else_block.map(|b| self::block(b, resolve)).transpose()?,
                    },
                    StmtKind::While { condition, body } => StmtKind::While {
                        condition,
                        body: self::block(body, resolve)?,
                    },
                    StmtKind::Return(value) => StmtKind::Return(value),
                    StmtKind::Print(value) => StmtKind::Print(value),
                    StmtKind::Expr(value) => StmtKind::Expr(value),
                    StmtKind::Block(body) => StmtKind::Block(self::block(body, resolve)?),
                    StmtKind::TensorScope {
                        budget,
                        body,
                        error_name,
                        error_span,
                        handler,
                    } => StmtKind::TensorScope {
                        budget,
                        body: self::block(body, resolve)?,
                        error_name,
                        error_span,
                        handler: self::block(handler, resolve)?,
                    },
                    StmtKind::Break => StmtKind::Break,
                    StmtKind::Continue => StmtKind::Continue,
                };
                Ok(Stmt {
                    kind,
                    span: stmt.span,
                })
            })
            .collect::<Result<_, E>>()?,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::convert::Infallible;

    const SOURCE: &str = r#"
        import contracts;
        export record Packet { number: i64, samples: [f64; 2] }
        export union Choice { Value { packet: Packet }, Empty {} }
        export trait Read { fn read(value: Self, bias: i64) -> i64; }
        impl Read for Packet { read = read_packet; }
        fn read_packet(value: Packet, bias: i64) -> i64 { return value.number + bias; }
        fn identity<T>(value: T) -> T { return identity::<T>(value); }
        fn all_paths(value: Choice) -> i64 {
            let mut n: i64 = identity::<i64>(1);
            let mut numbers: [i64; 2] = [1, 2];
            numbers[0] = identity::<i64>(2);
            let view: &[i64] = &numbers[0..1];
            print(view[0]);
            if n == 0 { let yes: () = (); } else { let no: bool = false; }
            while n < 3 {
                let step: i64 = 1;
                n = n + step;
                if n == 2 { continue; } else { break; }
            }
            { let nested: f64 = 2.0; }
            match value {
                Choice::Value { packet: owner } => {
                    let moved: Packet = owner;
                    return read_packet(moved, n);
                },
                Choice::Empty {} => {
                    let fallback: i64 = 0;
                    return fallback;
                }
            }
        }
        fn recovery() -> i64 {
            tensor_scope(64) { let protected: i64 = 1; }
            catch (failure: tensor_error) { let recovered: i64 = 2; }
            return 0;
        }
    "#;

    #[test]
    fn annotation_boundary_preserves_complete_source_structure() {
        let program = crate::parser::parse(SOURCE).unwrap();
        let before = format!("{program:?}");
        let source: SourceProgram = program
            .try_map_annotations(|ty, _| Ok::<_, Infallible>(TypeExpr::Concrete(ty)))
            .unwrap();
        let resolved = source
            .try_map_annotations(|ty, span| ty.concrete().ok_or(span))
            .unwrap();
        assert_eq!(format!("{resolved:?}"), before);
    }

    #[test]
    fn resolver_sees_fields_traits_targets_and_every_nested_declaration() {
        let program = crate::parser::parse(SOURCE).unwrap();
        let mut sites = Vec::new();
        let source: SourceProgram = program
            .try_map_annotations(|ty, span| {
                sites.push(SOURCE[span.start..span.end].to_owned());
                Ok::<_, Infallible>(ty.into())
            })
            .unwrap();
        for name in [
            "number",
            "samples",
            "packet",
            "bias",
            "Read",
            "n",
            "numbers",
            "view",
            "yes",
            "no",
            "step",
            "nested",
            "moved",
            "fallback",
            "protected",
            "recovered",
        ] {
            assert!(sites.iter().any(|site| site == name), "missing {name}");
        }
        // Templates and expression arguments do not belong to this ordinary
        // declaration boundary. Their actual opaque proof remains mandatory.
        assert!(!sites.iter().any(|site| site == "identity"));
        assert!(matches!(
            source.generic_functions[0].function.params[0].ty,
            TypeExpr::Parameter(_)
        ));
        assert!(matches!(
            source.traits[0].methods[0].params[0].ty,
            TraitType::SelfType
        ));
    }

    #[test]
    fn unresolved_source_field_cannot_silently_become_concrete() {
        let program = crate::parser::parse(SOURCE).unwrap();
        let mut source: SourceProgram = program
            .try_map_annotations(|ty, _| Ok::<_, Infallible>(ty.into()))
            .unwrap();
        let span = source.records[0].fields[0].name_span;
        source.records[0].fields[0].ty = source.generic_functions[0].function.params[0].ty;
        assert!(matches!(
            source.records[0].fields[0].ty,
            TypeExpr::Parameter(_)
        ));
        let failure = source.try_map_annotations(|ty, site| ty.concrete().ok_or(site));
        assert_eq!(failure.unwrap_err(), span);
    }
}
