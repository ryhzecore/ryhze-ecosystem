//! Native object receipts independently rehashed from their published graphs.
mod support;
use std::fs;
use std::path::PathBuf;
use std::process::Output;
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT: AtomicUsize = AtomicUsize::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "axeon-nominal-metadata-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
    fn write(&self, name: &str, source: &str) {
        fs::write(self.0.join(name), source).unwrap();
    }
    fn project(&self) {
        for (name, source) in SOURCES {
            self.write(name, source);
        }
    }
    fn command(&self, args: &[&str]) -> Output {
        let output = support::bounded_with_kill(env!("CARGO_BIN_EXE_axeon"), "45s", "2s")
            .args(args)
            .current_dir(&self.0)
            .output()
            .unwrap();
        assert!(output.status.success(), "{args:?}: {output:?}");
        assert!(output.stderr.is_empty(), "{args:?}: {output:?}");
        output
    }
    fn object(&self, tag: &str) {
        self.command(&[
            "emit-object",
            "main.ax",
            "--opt",
            "0",
            "-o",
            &format!("{tag}.o"),
        ]);
    }
    fn python(&self, source: &str, args: &[&str]) {
        let output = support::bounded_with_kill(support::python(), "15s", "2s")
            .args(["-B", "-c", source])
            .args(args)
            .current_dir(&self.0)
            .output()
            .unwrap();
        assert!(output.status.success(), "{output:?}");
        assert!(output.stderr.is_empty(), "{output:?}");
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

const CONTRACTS: &str = "export trait Read{fn read(value:Self)->i64;} export trait Tag{fn tag(value:Self)->i64;} fn read_i64(value:i64)->i64{return value;} impl Read for i64{read=read_i64;} impl Tag for i64{tag=read_i64;}";
const TYPES: &str = "import contracts; export record Cell<T>{value:T} export union Choice<T>{Some{value:T},None{}} export record Plain{value:i64} export record Envelope{cell:Cell<i64>} export record Checked<T:contracts::Read>{value:T} fn fee()->i64{return 0;} export fn make(value:i64)->Cell<i64>{return Cell::<i64>{value:value+fee()};} export fn keep<T>(value:Cell<T>)->Cell<T>{return value;}";
const ENTRY: &str = "import types; import scalar; import plain_api; import generic_api; fn main()->i64{let cell:types::Cell<i64>=types::make(40);let next:types::Cell<i64>=types::keep::<i64>(cell);return next.value+scalar::value();}";
const SOURCES: &[(&str, &str)] = &[
    ("main.ax", ENTRY),
    ("types.ax", TYPES),
    ("contracts.ax", CONTRACTS),
    ("scalar.ax", "export fn value()->i64{return 2;}"),
    (
        "plain_api.ax",
        "import types; export fn read(value:types::Plain)->i64{return value.value;}",
    ),
    (
        "generic_api.ax",
        "export fn identity<T>(value:T)->T{return value;}",
    ),
];

// Rebuild reachable subgraphs from explicit edges, derive those edges independently
// from the type/trait descriptors, and verify every schema-5 record and outer hash.
const REHASH: &str = r#"
import hashlib,json,sys
from pathlib import Path
compact=lambda x:json.dumps(x,separators=(',',':'),ensure_ascii=False)
digest=lambda tag,x:hashlib.sha256((tag+'\n'+compact(x)).encode()).hexdigest()
key=lambda r:(r['kind'],r['module'],r['name'])
def references(x):
    if isinstance(x,dict):
        if set(x)=={'kind','module','name'}: return {key(x)}
        return set().union(*(references(v) for v in x.values()))
    if isinstance(x,list): return set().union(*(references(v) for v in x))
    return set()
def forbidden(x):
    if isinstance(x,dict):
        assert not ({'owner','index','source_id','offset','binder'} & set(x)),x
        for v in x.values(): forbidden(v)
    elif isinstance(x,list):
        for v in x: forbidden(v)
for argument in sys.argv[1:]:
    d=json.loads(Path(argument).read_text())
    modules={m['path']:m for m in d['modules']}
    for m in d['modules']:
        if m['interface_schema']==5:
            graph=m['canonical_graph']; forbidden(graph)
            assert [key(n['reference']) for n in graph]==sorted(key(n['reference']) for n in graph)
            nodes={key(n['reference']):n for n in graph}; assert len(nodes)==len(graph)
            for k,n in nodes.items():
                assert key(n['definition'])==k
                assert [key(r) for r in n['dependencies']]==sorted(references(n['definition']))
                assert references(n['definition'])<=nodes.keys()
            def reachable(roots):
                seen=set(); todo=list(roots)
                while todo:
                    r=todo.pop()
                    if r not in seen:
                        seen.add(r); todo.extend(key(v) for v in nodes[r]['dependencies'])
                return [nodes[r] for r in sorted(seen)]
            for item in m['type_exports']+m['generic_nominal_exports']+m['trait_exports']:
                ref=item['reference']; assert item['definition']==nodes[key(ref)]['definition']
                assert item['definition_sha256']==digest('axeon-public-definition-v5',{'root':ref,'nodes':reachable([key(ref)])})
            for item in m['exports']:
                roots=[key(r) for r in item['references']]
                assert roots==sorted(references(item['signature']))
                assert item['signature_sha256']==digest('axeon-export-signature-v5',{'definition':item['signature'],'nodes':reachable(roots)})
            for item in m['generic_exports']:
                roots=[key(r) for r in item['references']]
                assert roots==sorted(references(item['definition']))
                assert item['definition_sha256']==digest('axeon-export-generic-v5',{'definition':item['definition'],'nodes':reachable(roots)})
            shape={'module':m['path'],'types':m['type_exports'],'nominals':m['generic_nominal_exports'],'functions':m['exports'],'traits':m['trait_exports'],'implementations':m['implementations'],'generics':m['generic_exports'],'graph':graph}
            assert m['interface_definition']==shape
            assert m['interface_sha256']==digest('axeon-module-interface-v5',shape)
            forbidden(shape)
        inputs={k:m[k] for k in ['path','content_sha256','interface_sha256','imports']}
        assert m['dependency_sha256']==digest('axeon-module-dependency-v1',inputs)
        for edge in m['imports']:
            assert edge['interface_sha256']==modules[edge['path']]['interface_sha256']
            assert edge['dependency_sha256']==modules[edge['path']]['dependency_sha256']
    assert d['module_graph_sha256']==digest('axeon-module-graph-v1',d['modules'])
    assert d['complete_build_sha256']==digest('axeon-complete-build-v1',d['build_inputs'])
    assert str(Path.cwd()) not in compact(d)
"#;

#[test]
fn schema5_graphs_independently_rehash_and_coexist_with_unchanged_old_schemas() {
    let fixture = Fixture::new();
    fixture.project();
    fixture.object("baseline");
    assert_eq!(
        fixture.command(&["interpret", "main.ax"]).stdout,
        b"result: 42\n"
    );
    fixture.python(REHASH, &["baseline.o.json"]);
    fixture.python(r#"
import json,hashlib
from pathlib import Path
d=json.loads(Path('baseline.o.json').read_text()); m={x['path']:x for x in d['modules']}
assert {p:x['interface_schema'] for p,x in m.items()}=={'main.ax':1,'types.ax':5,'contracts.ax':3,'scalar.ax':1,'plain_api.ax':2,'generic_api.ax':4}
compact=lambda x:json.dumps(x,separators=(',',':'))
digest=lambda tag,x:hashlib.sha256((tag+'\n'+compact(x)).encode()).hexdigest()
plain={'kind':'record','module':'types.ax','name':'Plain','fields':[{'name':'value','type':'i64'}]}
reference={'kind':'record','module':'types.ax','name':'Plain','definition_sha256':digest('axeon-export-type-v2',plain)}
signature={'name':'read','params':[reference],'result':'i64'}
assert m['plain_api.ax']['exports'][0]['signature']==signature
assert m['plain_api.ax']['interface_sha256']==digest('axeon-module-interface-v2',{'module':'plain_api.ax','types':[],'functions':[signature]})
assert [x['name'] for x in m['types.ax']['generic_nominal_exports']]==['Cell','Checked','Choice']
assert all('instance' not in compact(x) for x in m['types.ax']['canonical_graph'])
assert 'fee' not in compact(m['types.ax']['interface_definition'])
"#, &[]);
    // Unrelated private nominal syntax must not migrate or alter a scalar interface.
    fixture.write(
        "scalar.ax",
        "record Hidden<T>{value:T} export fn value()->i64{return 2;}",
    );
    fixture.object("private");
    fixture.python(REHASH, &["private.o.json"]);
    fixture.python(r#"
import json
from pathlib import Path
a,b=[{m['path']:m for m in json.loads(Path(p).read_text())['modules']} for p in ['baseline.o.json','private.o.json']]
assert a['scalar.ax']['interface_schema']==b['scalar.ax']['interface_schema']==1
assert a['scalar.ax']['interface_sha256']==b['scalar.ax']['interface_sha256']
assert a['scalar.ax']['dependency_sha256']!=b['scalar.ax']['dependency_sha256']
"#, &[]);
}

#[test]
fn relocation_and_parameter_renaming_preserve_structural_nominal_hashes() {
    let first = Fixture::new();
    first.project();
    first.object("baseline");
    let moved = Fixture::new();
    moved.project();
    moved.object("baseline");
    first.python(
        REHASH,
        &[
            "baseline.o.json",
            moved.0.join("baseline.o.json").to_str().unwrap(),
        ],
    );
    first.python(
        r#"
import json,sys
from pathlib import Path
a=json.loads(Path('baseline.o.json').read_text());b=json.loads(Path(sys.argv[1]).read_text())
assert a['modules']==b['modules']
assert a['complete_build_sha256']==b['complete_build_sha256']
"#,
        &[moved.0.join("baseline.o.json").to_str().unwrap()],
    );
    first.write(
        "types.ax",
        &TYPES.replace("record Cell<T>{value:T}", "record Cell<Item>{value:Item}"),
    );
    first.object("renamed");
    first.python(REHASH, &["renamed.o.json"]);
    first.python(r#"
import json
from pathlib import Path
a,b=[{m['path']:m for m in json.loads(Path(p).read_text())['modules']} for p in ['baseline.o.json','renamed.o.json']]
assert a['types.ax']['interface_sha256']==b['types.ax']['interface_sha256']
assert a['types.ax']['dependency_sha256']!=b['types.ax']['dependency_sha256']
assert a['types.ax']['canonical_graph']==b['types.ax']['canonical_graph']
"#, &[]);
}

#[test]
fn fields_bounds_arguments_template_bodies_and_private_dependencies_invalidate_correctly() {
    let fixture = Fixture::new();
    fixture.project();
    fixture.object("baseline");
    for (tag, source) in [
        (
            "field",
            TYPES.replace(
                "export record Envelope{cell:Cell<i64>}",
                "export record Envelope{cell:Cell<i64>,stamp:()}",
            ),
        ),
        (
            "bound",
            TYPES.replace("Checked<T:contracts::Read>", "Checked<T:contracts::Tag>"),
        ),
        (
            "argument",
            TYPES.replace(
                "export record Envelope{cell:Cell<i64>}",
                "export record Envelope{cell:Cell<f64>}",
            ),
        ),
        (
            "body",
            TYPES.replace("{return value;}", "{let next:Cell<T>=value;return next;}"),
        ),
        (
            "helper",
            TYPES.replace("fn fee()->i64{return 0;}", "fn fee()->i64{return 1;}"),
        ),
    ] {
        fixture.write("types.ax", &source);
        fixture.object(tag);
    }
    fixture.python(
        REHASH,
        &[
            "baseline.o.json",
            "field.o.json",
            "bound.o.json",
            "argument.o.json",
            "body.o.json",
            "helper.o.json",
        ],
    );
    fixture.python(r#"
import json
from pathlib import Path
load=lambda tag:{m['path']:m for m in json.loads(Path(tag+'.o.json').read_text())['modules']}
a=load('baseline')['types.ax']
for tag in ['field','bound','argument','body']:
    b=load(tag)['types.ax'];assert a['interface_sha256']!=b['interface_sha256'],tag
    assert a['dependency_sha256']!=b['dependency_sha256'],tag
b=load('helper')['types.ax'];assert a['interface_sha256']==b['interface_sha256']
assert a['dependency_sha256']!=b['dependency_sha256']
named=lambda m,field:{x['name']:x for x in m[field]}
assert named(a,'generic_nominal_exports')['Cell']['definition_sha256']==named(load('body')['types.ax'],'generic_nominal_exports')['Cell']['definition_sha256']
assert named(a,'generic_exports')['keep']['definition_sha256']!=named(load('body')['types.ax'],'generic_exports')['keep']['definition_sha256']
"#, &[]);
}

#[test]
fn reachable_nominal_field_changes_invalidate_importing_function_signatures() {
    let fixture = Fixture::new();
    fixture.write("types.ax", "export record Cell<T>{value:T}");
    fixture.write(
        "api.ax",
        "import types; export fn forward(value:types::Cell<i64>)->types::Cell<i64>{return value;}",
    );
    fixture.write("main.ax", "import api; fn main()->i64{return 0;}");
    fixture.object("baseline");
    fixture.write("types.ax", "export record Cell<T>{value:T,stamp:()}");
    fixture.object("changed");
    fixture.python(REHASH, &["baseline.o.json", "changed.o.json"]);
    fixture.python(r#"
import json
from pathlib import Path
a,b=[{m['path']:m for m in json.loads(Path(p).read_text())['modules']} for p in ['baseline.o.json','changed.o.json']]
assert a['api.ax']['interface_schema']==b['api.ax']['interface_schema']==5
assert a['api.ax']['exports'][0]['signature']==b['api.ax']['exports'][0]['signature']
assert a['api.ax']['exports'][0]['signature_sha256']!=b['api.ax']['exports'][0]['signature_sha256']
assert a['api.ax']['interface_sha256']!=b['api.ax']['interface_sha256']
"#, &[]);
}

#[test]
fn trait_signature_bound_cycles_terminate_and_applied_impls_are_public_coherence_inputs() {
    let fixture = Fixture::new();
    fixture.write("main.ax", "export record Cell<T:Read>{value:T} export trait Read{fn read(value:Self)->i64;fn wrap(value:Self)->Cell<i64>;} fn read_i64(value:i64)->i64{return value;} fn wrap_i64(value:i64)->Cell<i64>{return Cell::<i64>{value:value};} impl Read for i64{read=read_i64;wrap=wrap_i64;} export trait Inspect{fn inspect(value:Self)->i64;} fn inspect_cell(value:Cell<i64>)->i64{return value.value;} impl Inspect for Cell<i64>{inspect=inspect_cell;} fn main()->i64{return 0;}");
    fixture.object("cycle");
    fixture.python(REHASH, &["cycle.o.json"]);
    fixture.python(r#"
import json
from pathlib import Path
m=json.loads(Path('cycle.o.json').read_text())['modules'][0]
assert m['interface_schema']==5
nodes={x['reference']['name']:x for x in m['canonical_graph']}
assert 'Read' in [r['name'] for r in nodes['Cell']['dependencies']]
assert 'Cell' in [r['name'] for r in nodes['Read']['dependencies']]
impl=next(x for x in m['implementations'] if x['trait']['name']=='Inspect')
assert impl['target']=={'declaration':{'kind':'record','module':'main.ax','name':'Cell'},'arguments':['i64']}
"#, &[]);
}
