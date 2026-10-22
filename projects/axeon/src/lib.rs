pub mod ast;
pub mod check;
pub mod diagnostic;
pub mod interpreter;
pub mod ir;
pub mod lexer;
pub mod llvm;
pub mod nominal;
pub mod parser;
pub mod project;
pub mod tooling;
pub mod verify;

pub fn compile(source: &str) -> Result<ir::Program, Vec<diagnostic::Diagnostic>> {
    let program = parser::parse_source(source)?;
    if program.applications.is_empty()
        && program.generic_records.is_empty()
        && program.generic_unions.is_empty()
    {
        check::check(&program.resolve_ordinary_annotations()?)
    } else {
        check::check_source(&program)
    }
}
