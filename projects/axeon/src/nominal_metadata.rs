//! Canonical public declaration graphs for interfaces containing nominal applications.
//! Graph edges contain logical references; definition hashes never recurse through
//! other definition hashes, including trait/bound reference cycles.
use super::{quote, sha256, ModuleEvidence, TraitEvidence, TypeEvidence};
use axeon::ast::{self, NominalId, NominalKind, RecordId, Type, TypeExpr};
use axeon::diagnostic::SourceId;
use axeon::project::{Module, Project};
use std::collections::{BTreeMap, BTreeSet, VecDeque};

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct Key {
    kind: &'static str,
    module: String,
    name: String,
}
impl Key {
    fn json(&self) -> String {
        format!(
            "{{\"kind\":{},\"module\":{},\"name\":{}}}",
            quote(self.kind),
            quote(&self.module),
            quote(&self.name)
        )
    }
}
#[derive(Clone)]
struct Node {
    definition: String,
    dependencies: BTreeSet<Key>,
}
impl Node {
    fn json(&self, key: &Key) -> String {
        format!(
            "{{\"reference\":{},\"definition\":{},\"dependencies\":[{}]}}",
            key.json(),
            self.definition,
            self.dependencies
                .iter()
                .map(Key::json)
                .collect::<Vec<_>>()
                .join(",")
        )
    }
}
#[derive(Clone, Copy)]
struct Binder {
    id: RecordId,
    count: usize,
}
#[derive(Clone, Copy)]
enum Declaration {
    Nominal(RecordId),
    Trait(RecordId),
}
struct Graph<'a> {
    project: &'a Project,
    pending: VecDeque<(Key, Declaration)>,
    seen: BTreeSet<Key>,
    nodes: BTreeMap<Key, Node>,
    needs_v5: bool,
    expression_nodes: usize,
}
impl<'a> Graph<'a> {
    fn new(project: &'a Project) -> Self {
        Self {
            project,
            pending: VecDeque::new(),
            seen: BTreeSet::new(),
            nodes: BTreeMap::new(),
            needs_v5: false,
            expression_nodes: 0,
        }
    }
    fn path(&self, source: SourceId) -> Result<&str, String> {
        self.project
            .sources
            .get(source)
            .map(|file| file.logical_path.as_str())
            .ok_or_else(|| "public graph source is unavailable".into())
    }
    fn enqueue(
        &mut self,
        key: Key,
        declaration: Declaration,
        references: &mut BTreeSet<Key>,
    ) -> Result<Key, String> {
        references.insert(key.clone());
        if self.seen.insert(key.clone()) {
            if self.seen.len() > 320 {
                return Err(
                    "public declaration graph exceeds 256 nominal plus 64 trait declarations"
                        .into(),
                );
            }
            self.pending.push_back((key.clone(), declaration));
        }
        Ok(key)
    }
    fn nominal(&mut self, id: RecordId, references: &mut BTreeSet<Key>) -> Result<Key, String> {
        let declaration = self
            .project
            .nominal_session()
            .and_then(|session| session.declaration(id))
            .ok_or("public nominal declaration is unavailable")?;
        if !declaration.exported {
            return Err("public graph contains a private nominal declaration".into());
        }
        self.needs_v5 |= !declaration.parameters.is_empty();
        let key = Key {
            kind: match declaration.header.kind {
                NominalKind::Record => "record",
                NominalKind::Union => "union",
            },
            module: self.path(id.source)?.into(),
            name: declaration.name,
        };
        self.enqueue(key, Declaration::Nominal(id), references)
    }
    fn contract(&self, id: RecordId) -> Result<&ast::Trait, String> {
        self.project
            .modules
            .iter()
            .flat_map(|module| &module.trait_exports)
            .find(|contract| contract.id == id)
            .ok_or_else(|| "public trait declaration is unavailable".into())
    }
    fn trait_ref(&mut self, id: RecordId, references: &mut BTreeSet<Key>) -> Result<Key, String> {
        let key = Key {
            kind: "trait",
            module: self.path(id.source)?.into(),
            name: self.contract(id)?.name.clone(),
        };
        self.enqueue(key, Declaration::Trait(id), references)
    }
    fn ty(
        &mut self,
        ty: TypeExpr,
        binder: Option<Binder>,
        references: &mut BTreeSet<Key>,
        depth: usize,
    ) -> Result<String, String> {
        self.expression_nodes += 1;
        if self.expression_nodes > 131_072 || depth > 32 {
            return Err("public type descriptor exceeds bounded node or application depth".into());
        }
        match ty {
            TypeExpr::Parameter(parameter) => {
                if !binder.is_some_and(|binder| {
                    parameter.binder == binder.id && parameter.index < binder.count
                }) {
                    return Err(
                        "public signature uses a foreign or invalid parameter identity".into(),
                    );
                }
                Ok(format!("{{\"parameter\":{}}}", parameter.index))
            }
            TypeExpr::Application(id) => {
                let application = self
                    .project
                    .nominal_session()
                    .and_then(|session| session.application(id))
                    .ok_or("public application owner or index is invalid")?;
                self.needs_v5 = true;
                self.application(
                    application.declaration,
                    application.kind,
                    &application.arguments,
                    binder,
                    references,
                    depth,
                )
            }
            TypeExpr::Concrete(Type::Record(id) | Type::Union(id)) => {
                let origin = self
                    .project
                    .nominal_session()
                    .and_then(|session| session.origin(id))
                    .ok_or("public nominal origin is unavailable")?;
                let expected = if matches!(ty, TypeExpr::Concrete(Type::Record(_))) {
                    NominalKind::Record
                } else {
                    NominalKind::Union
                };
                if origin.kind != expected {
                    return Err("public nominal origin has the wrong kind".into());
                }
                self.needs_v5 |= matches!(id, NominalId::Instance(_));
                self.application(
                    origin.declaration,
                    origin.kind,
                    &origin.arguments,
                    binder,
                    references,
                    depth,
                )
            }
            TypeExpr::Concrete(ty) => super::interface_type(self.project, ty, &[]),
        }
    }
    fn application(
        &mut self,
        id: RecordId,
        kind: NominalKind,
        arguments: &[TypeExpr],
        binder: Option<Binder>,
        references: &mut BTreeSet<Key>,
        depth: usize,
    ) -> Result<String, String> {
        let declaration = self
            .project
            .nominal_session()
            .and_then(|session| session.declaration(id))
            .ok_or("public application declaration is unavailable")?;
        if declaration.header.kind != kind || declaration.parameters.len() != arguments.len() {
            return Err("public application kind or arity is invalid".into());
        }
        let key = self.nominal(id, references)?;
        if arguments.is_empty() {
            return Ok(key.json());
        }
        let arguments = arguments
            .iter()
            .map(|argument| self.ty(*argument, binder, references, depth + 1))
            .collect::<Result<Vec<_>, _>>()?;
        Ok(format!(
            "{{\"declaration\":{},\"arguments\":[{}]}}",
            key.json(),
            arguments.join(",")
        ))
    }
    fn parameters(
        &mut self,
        parameters: &[ast::TypeParameter],
        source: SourceId,
        references: &mut BTreeSet<Key>,
    ) -> Result<String, String> {
        let mut descriptors = Vec::new();
        for parameter in parameters {
            let mut bounds = BTreeSet::new();
            for bound in &parameter.bounds {
                let id = self
                    .project
                    .trait_definition(source, &bound.name)
                    .ok_or("public parameter bound is unavailable")?
                    .id;
                bounds.insert(self.trait_ref(id, references)?.json());
            }
            // Parameter spelling is not part of a structural nominal identity.
            descriptors.push(format!(
                "{{\"bounds\":[{}]}}",
                bounds.into_iter().collect::<Vec<_>>().join(",")
            ));
        }
        Ok(format!("[{}]", descriptors.join(",")))
    }
    fn fields(
        &mut self,
        fields: &[ast::RecordField<TypeExpr>],
        binder: Option<Binder>,
        references: &mut BTreeSet<Key>,
    ) -> Result<String, String> {
        let mut result = Vec::new();
        for field in fields {
            result.push(format!(
                "{{\"name\":{},\"type\":{}}}",
                quote(&field.name),
                self.ty(field.ty, binder, references, 0)?
            ));
        }
        Ok(format!("[{}]", result.join(",")))
    }
    fn finish(&mut self) -> Result<(), String> {
        while let Some((key, declaration)) = self.pending.pop_front() {
            let mut references = BTreeSet::new();
            let definition = match declaration {
                Declaration::Nominal(id) => {
                    let declaration = self
                        .project
                        .nominal_session()
                        .and_then(|session| session.declaration(id))
                        .ok_or("queued nominal declaration is unavailable")?;
                    let binder = Some(Binder {
                        id,
                        count: declaration.parameters.len(),
                    });
                    let parameters =
                        self.parameters(&declaration.parameters, id.source, &mut references)?;
                    let shape = match declaration.header.kind {
                        NominalKind::Record => format!(
                            "\"fields\":{}",
                            self.fields(&declaration.fields, binder, &mut references)?
                        ),
                        NominalKind::Union => {
                            let mut variants = Vec::new();
                            for variant in &declaration.variants {
                                variants.push(format!(
                                    "{{\"name\":{},\"fields\":{}}}",
                                    quote(&variant.name),
                                    self.fields(&variant.fields, binder, &mut references)?
                                ));
                            }
                            format!("\"variants\":[{}]", variants.join(","))
                        }
                    };
                    format!("{{\"kind\":{},\"module\":{},\"name\":{},\"parameters\":{parameters},{shape}}}", quote(key.kind), quote(&key.module), quote(&key.name))
                }
                Declaration::Trait(id) => {
                    let contract = self.contract(id)?.clone();
                    let mut methods = Vec::new();
                    for method in &contract.methods {
                        let mut descriptor = |ty: ast::TraitType<Type>| match ty {
                            ast::TraitType::SelfType => Ok("{\"kind\":\"self\"}".to_owned()),
                            ast::TraitType::Concrete(ty) => {
                                self.ty(ty.into(), None, &mut references, 0)
                            }
                        };
                        let params = method
                            .params
                            .iter()
                            .map(|parameter| descriptor(parameter.ty))
                            .collect::<Result<Vec<_>, String>>()?;
                        methods.push(format!(
                            "{{\"name\":{},\"params\":[{}],\"result\":{}}}",
                            quote(&method.name),
                            params.join(","),
                            descriptor(method.return_type)?
                        ));
                    }
                    format!(
                        "{{\"kind\":\"trait\",\"module\":{},\"name\":{},\"methods\":[{}]}}",
                        quote(&key.module),
                        quote(&key.name),
                        methods.join(",")
                    )
                }
            };
            self.nodes.insert(
                key,
                Node {
                    definition,
                    dependencies: references,
                },
            );
        }
        Ok(())
    }
    fn reachable(&self, roots: &BTreeSet<Key>) -> Result<String, String> {
        let mut reached = BTreeSet::new();
        let mut pending: Vec<_> = roots.iter().cloned().collect();
        while let Some(key) = pending.pop() {
            if reached.insert(key.clone()) {
                pending.extend(
                    self.nodes
                        .get(&key)
                        .ok_or("public graph edge has no descriptor")?
                        .dependencies
                        .iter()
                        .cloned(),
                );
            }
        }
        Ok(format!(
            "[{}]",
            reached
                .iter()
                .map(|key| self.nodes[key].json(key))
                .collect::<Vec<_>>()
                .join(",")
        ))
    }
    fn definition_record(&self, key: &Key) -> Result<String, String> {
        let nodes = self.reachable(&BTreeSet::from([key.clone()]))?;
        let inputs = format!("{{\"root\":{},\"nodes\":{nodes}}}", key.json());
        let digest = sha256(format!("axeon-public-definition-v5\n{inputs}").as_bytes())?;
        Ok(format!(
            "{{\"name\":{},\"reference\":{},\"definition\":{},\"definition_sha256\":{}}}",
            quote(&key.name),
            key.json(),
            self.nodes[key].definition,
            quote(&digest)
        ))
    }
    fn signature_digest(
        &self,
        tag: &str,
        definition: &str,
        references: &BTreeSet<Key>,
    ) -> Result<String, String> {
        let nodes = self.reachable(references)?;
        sha256(format!("{tag}\n{{\"definition\":{definition},\"nodes\":{nodes}}}").as_bytes())
    }
}

pub(super) fn type_needs_v5(project: &Project, ty: Type) -> Result<bool, String> {
    if project.nominal_session().is_none() {
        return Ok(false);
    }
    let mut graph = Graph::new(project);
    graph.ty(ty.into(), None, &mut BTreeSet::new(), 0)?;
    graph.finish()?;
    Ok(graph.needs_v5)
}
pub(super) fn trait_needs_v5(project: &Project, id: RecordId) -> Result<bool, String> {
    if project.nominal_session().is_none() {
        return Ok(false);
    }
    let mut graph = Graph::new(project);
    graph.trait_ref(id, &mut BTreeSet::new())?;
    graph.finish()?;
    Ok(graph.needs_v5)
}

struct Descriptor {
    name: String,
    definition: String,
    references: BTreeSet<Key>,
}
struct Public {
    graph: String,
    types: String,
    nominals: String,
    traits: String,
    functions: String,
    generics: String,
    implementations: String,
    interface: String,
}

fn public(project: &Project, module: &Module) -> Result<Option<Public>, String> {
    if project.nominal_session().is_none() {
        return Ok(None);
    }
    let mut graph = Graph::new(project);
    let mut roots = BTreeSet::new();
    let mut types = Vec::new();
    for export in &module.type_exports {
        let id = match export.ty {
            Type::Record(id) | Type::Union(id) => id
                .declaration()
                .ok_or("synthetic type was exported as a declaration")?,
            _ => return Err("non-nominal type export".into()),
        };
        types.push(graph.nominal(id, &mut roots)?);
    }
    let mut nominals = Vec::new();
    for id in &module.generic_nominal_exports {
        nominals.push(graph.nominal(*id, &mut roots)?);
    }
    let mut traits = Vec::new();
    for contract in &module.trait_exports {
        traits.push(graph.trait_ref(contract.id, &mut roots)?);
    }
    let mut functions = Vec::new();
    for export in &module.exports {
        let mut references = BTreeSet::new();
        let params = export
            .params
            .iter()
            .map(|ty| graph.ty((*ty).into(), None, &mut references, 0))
            .collect::<Result<Vec<_>, _>>()?;
        let result = graph.ty(export.return_type.into(), None, &mut references, 0)?;
        functions.push(Descriptor {
            name: export.name.clone(),
            definition: format!(
                "{{\"name\":{},\"params\":[{}],\"result\":{result}}}",
                quote(&export.name),
                params.join(",")
            ),
            references,
        });
    }
    let mut generics = Vec::new();
    for export in &module.generic_exports {
        let source = project
            .source_program(module.source)
            .ok_or("public generic source is unavailable")?;
        let function = source
            .generic_functions
            .iter()
            .find(|template| template.function.name == export.name)
            .ok_or("public generic binder is unavailable")?;
        let binder = Binder {
            id: RecordId {
                source: module.source,
                offset: function.function.name_span.start,
            },
            count: export.parameters.len(),
        };
        let mut references = BTreeSet::new();
        let parameters = graph.parameters(&export.parameters, module.source, &mut references)?;
        let params = export
            .params
            .iter()
            .map(|ty| graph.ty(*ty, Some(binder), &mut references, 0))
            .collect::<Result<Vec<_>, _>>()?;
        let result = graph.ty(export.return_type, Some(binder), &mut references, 0)?;
        let file = project
            .sources
            .get(module.source)
            .ok_or("public generic source is unavailable")?;
        if export.body_span.source != module.source {
            return Err("public generic body has a foreign source".into());
        }
        let body = file
            .text
            .get(export.body_span.start..export.body_span.end)
            .ok_or("public generic body span is invalid")?;
        let body_sha = sha256(format!("axeon-generic-body-v1\n{body}").as_bytes())?;
        generics.push(Descriptor { name: export.name.clone(), definition: format!("{{\"module\":{},\"name\":{},\"parameters\":{parameters},\"params\":[{}],\"result\":{result},\"body_sha256\":{}}}", quote(&file.logical_path), quote(&export.name), params.join(","), quote(&body_sha)), references });
    }
    let mut implementations = Vec::new();
    for implementation in &module.implementations {
        let contract = project
            .trait_definition(module.source, &implementation.trait_name)
            .ok_or("implementation contract is unavailable")?;
        if !contract.exported || !project.type_is_public(implementation.target) {
            continue;
        }
        let mut references = BTreeSet::new();
        let contract = graph.trait_ref(contract.id, &mut references)?;
        let target = graph.ty(implementation.target.into(), None, &mut references, 0)?;
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
            .collect::<Vec<_>>()
            .join(",");
        implementations.push(format!(
            "{{\"trait\":{},\"target\":{target},\"module\":{},\"methods\":[{methods}]}}",
            contract.json(),
            quote(graph.path(module.source)?)
        ));
        roots.extend(references);
    }
    for descriptor in functions.iter().chain(&generics) {
        roots.extend(descriptor.references.iter().cloned());
    }
    graph.finish()?;
    if !graph.needs_v5 {
        return Ok(None);
    }
    types.sort();
    nominals.sort();
    traits.sort();
    implementations.sort();
    functions.sort_by(|left, right| left.name.cmp(&right.name));
    generics.sort_by(|left, right| left.name.cmp(&right.name));
    let records = |keys: &[Key]| -> Result<String, String> {
        Ok(format!(
            "[{}]",
            keys.iter()
                .map(|key| graph.definition_record(key))
                .collect::<Result<Vec<_>, _>>()?
                .join(",")
        ))
    };
    let types = records(&types)?;
    let nominals = records(&nominals)?;
    let traits = records(&traits)?;
    let mut function_records = Vec::new();
    for descriptor in functions {
        let digest = graph.signature_digest(
            "axeon-export-signature-v5",
            &descriptor.definition,
            &descriptor.references,
        )?;
        function_records.push(format!(
            "{{\"name\":{},\"signature\":{},\"signature_sha256\":{},\"references\":[{}]}}",
            quote(&descriptor.name),
            descriptor.definition,
            quote(&digest),
            descriptor
                .references
                .iter()
                .map(Key::json)
                .collect::<Vec<_>>()
                .join(",")
        ));
    }
    let mut generic_records = Vec::new();
    for descriptor in generics {
        let digest = graph.signature_digest(
            "axeon-export-generic-v5",
            &descriptor.definition,
            &descriptor.references,
        )?;
        generic_records.push(format!(
            "{{\"name\":{},\"definition\":{},\"definition_sha256\":{},\"references\":[{}]}}",
            quote(&descriptor.name),
            descriptor.definition,
            quote(&digest),
            descriptor
                .references
                .iter()
                .map(Key::json)
                .collect::<Vec<_>>()
                .join(",")
        ));
    }
    let graph = graph.reachable(&roots)?;
    let functions = format!("[{}]", function_records.join(","));
    let generics = format!("[{}]", generic_records.join(","));
    let implementations = format!("[{}]", implementations.join(","));
    let path = project
        .sources
        .get(module.source)
        .ok_or("module source is unavailable")?;
    let interface = format!("{{\"module\":{},\"types\":{types},\"nominals\":{nominals},\"functions\":{functions},\"traits\":{traits},\"implementations\":{implementations},\"generics\":{generics},\"graph\":{graph}}}", quote(&path.logical_path));
    Ok(Some(Public {
        graph,
        types,
        nominals,
        traits,
        functions,
        generics,
        implementations,
        interface,
    }))
}

pub(super) fn module_evidence(
    source: SourceId,
    project: &Project,
    definitions: &[TypeEvidence],
    traits: &[TraitEvidence],
    memo: &mut BTreeMap<SourceId, ModuleEvidence>,
) -> Result<Option<ModuleEvidence>, String> {
    let module = project
        .modules
        .iter()
        .find(|module| module.source == source)
        .ok_or("module source is unavailable")?;
    let Some(public) = public(project, module)? else {
        return Ok(None);
    };
    let file = project
        .sources
        .get(source)
        .ok_or("module source is unavailable")?;
    let content = sha256(file.text.as_bytes())?;
    let interface = sha256(format!("axeon-module-interface-v5\n{}", public.interface).as_bytes())?;
    let mut imports: Vec<_> = module.imports.iter().collect();
    imports.sort_by(|left, right| left.name.cmp(&right.name));
    let mut imported = Vec::new();
    for import in imports {
        let dependency = super::module_evidence(import.source, project, definitions, traits, memo)?;
        imported.push(format!(
            "{{\"name\":{},\"path\":{},\"interface_sha256\":{},\"dependency_sha256\":{}}}",
            quote(&import.name),
            quote(&dependency.path),
            quote(&dependency.interface),
            quote(&dependency.dependency)
        ));
    }
    let imports = format!("[{}]", imported.join(","));
    let inputs = format!(
        "{{\"path\":{},\"content_sha256\":{},\"interface_sha256\":{},\"imports\":{imports}}}",
        quote(&file.logical_path),
        quote(&content),
        quote(&interface)
    );
    let dependency = sha256(format!("axeon-module-dependency-v1\n{inputs}").as_bytes())?;
    Ok(Some(ModuleEvidence { path: file.logical_path.clone(), content, interface_schema: 5, interface, dependency, exports: public.functions, type_exports: public.types, trait_fields: format!(",\"trait_exports\":{},\"implementations\":{}", public.traits, public.implementations), generic_fields: format!(",\"generic_exports\":{},\"generic_nominal_exports\":{},\"canonical_graph\":{},\"interface_definition\":{}", public.generics, public.nominals, public.graph, public.interface), imports }))
}
