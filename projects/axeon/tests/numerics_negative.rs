//! Independent strict-f64 rejection fixtures and resource-boundary regressions.
use axeon::diagnostic::{Diagnostic, Span};
use std::fmt::Write;

fn source(body: &str) -> String {
    format!("fn main() -> i64 {{ {body} return 0; }}")
}

fn rejects(program: &str, codes: &[&str]) -> Vec<Diagnostic> {
    let errors =
        axeon::compile(program).expect_err("invalid numerical program must not produce IR");
    assert_eq!(
        errors.iter().map(|error| error.code).collect::<Vec<_>>(),
        codes,
        "{program}\n{errors:#?}"
    );
    assert_eq!(
        axeon::compile(program).unwrap_err(),
        errors,
        "diagnostic ordering must be deterministic"
    );
    for error in &errors {
        for span in
            std::iter::once(error.span).chain(error.related.iter().map(|related| related.span))
        {
            assert!(span.start <= span.end && span.end <= program.len());
            assert!(program.is_char_boundary(span.start) && program.is_char_boundary(span.end));
        }
    }
    errors
}

fn rejects_with_code(program: &str, code: &str) {
    let errors = axeon::compile(program).unwrap_err();
    assert!(
        errors.iter().any(|error| error.code == code),
        "{program}\n{errors:#?}"
    );
}

#[test]
fn malformed_and_overflowing_float_literals_cover_the_complete_spelling() {
    for (literal, code) in [
        ("1.", "E_LEX_NUMBER"),
        ("1.e2", "E_LEX_NUMBER"),
        ("1e", "E_LEX_NUMBER"),
        ("1e+", "E_LEX_NUMBER"),
        ("1E-", "E_LEX_NUMBER"),
        ("1.0e+", "E_LEX_NUMBER"),
        ("1e2e3", "E_LEX_NUMBER"),
        ("1.0f64", "E_LEX_NUMBER"),
        ("1.0f32", "E_LEX_NUMBER"),
        ("1_000.0", "E_LEX_NUMBER"),
        ("1.0_0", "E_LEX_NUMBER"),
        ("1e2_0", "E_LEX_NUMBER"),
        ("0x1.2p3", "E_LEX_NUMBER"),
        ("1.2.3", "E_LEX_NUMBER"),
        ("1e309", "E_FLOAT_RANGE"),
        ("1.7976931348623159e308", "E_FLOAT_RANGE"),
    ] {
        let prefix = "// λ 雪\n\u{2003}fn main()->i64 { let x:f64 = ";
        let program = format!("{prefix}{literal}; return 0; }}");
        let errors = rejects(&program, &[code]);
        assert_eq!(
            errors[0].span,
            Span::new(prefix.len(), prefix.len() + literal.len())
        );
        assert_eq!(&program[errors[0].span.start..errors[0].span.end], literal);
    }
    let program = source("let x:f64 = -1e309;");
    let errors = rejects(&program, &["E_FLOAT_RANGE"]);
    assert_eq!(&program[errors[0].span.start..errors[0].span.end], "1e309");
    let program = source("let x:f64 = .5;");
    let errors = rejects(&program, &["E_LEX_CHAR"]);
    assert_eq!(&program[errors[0].span.start..errors[0].span.end], ".");
}

#[test]
fn missing_exponent_ends_and_large_exponents_remain_bounded() {
    for literal in ["1e", "1e+", "1E-", "1.0e", "1.0E+"] {
        let program = format!("fn main()->i64 {{ let x:f64 = {literal}");
        let errors = rejects(&program, &["E_LEX_NUMBER"]);
        assert_eq!(errors[0].span.end, program.len());
        assert_eq!(&program[errors[0].span.start..], literal);
    }
    let overflow = format!("1e{}", "9".repeat(100_000));
    rejects(
        &source(&format!("let x:f64={overflow};")),
        &["E_FLOAT_RANGE"],
    );
    let underflow = format!("1e-{}", "9".repeat(100_000));
    axeon::compile(&source(&format!("let x:f64={underflow};"))).unwrap();
    let errors = axeon::compile(&"1e+ ".repeat(1000)).unwrap_err();
    assert_eq!(errors.len(), 64);
    for (index, error) in errors.iter().enumerate() {
        assert_eq!(error.code, "E_LEX_NUMBER");
        assert_eq!(error.span, Span::new(index * 4, index * 4 + 3));
    }
}

#[test]
fn mixed_numeric_operators_never_insert_implicit_conversions() {
    for operator in ["+", "-", "*", "/", "<", "<=", ">", ">=", "==", "!="] {
        for (left, right) in [("1.0", "2"), ("1", "2.0")] {
            let program = source(&format!("print({left} {operator} {right});"));
            let errors = rejects(&program, &["T009"]);
            assert_eq!(&program[errors[0].span.start..errors[0].span.end], right);
        }
    }
    for operator in ["%", "<<", ">>", "&", "|", "^", "&&", "||"] {
        let program = source(&format!("print(1.0 {operator} 2.0);"));
        let errors = rejects(&program, &["T009", "T009"]);
        assert_eq!(&program[errors[0].span.start..errors[0].span.end], "1.0");
        assert_eq!(&program[errors[1].span.start..errors[1].span.end], "2.0");
    }
    for expression in ["!1.0", "~1.0", "true + 1.0", "1.0 + false", "1.0 == false"] {
        rejects(&source(&format!("print({expression});")), &["T009"]);
    }
}

#[test]
fn f64_statements_calls_and_intrinsics_require_exact_types_and_arities() {
    for body in [
        "let x:f64=1;",
        "let x:i64=1.0;",
        "let x:bool=0.0;",
        "let mut x:f64=0.0; x=1;",
        "let mut x:i64=0; x=1.0;",
        "if 0.0 {}",
        "while 0.0 {}",
    ] {
        rejects(&source(body), &["T009"]);
    }
    rejects("fn main()->i64 { return 0.0; }", &["T009"]);
    rejects("fn main()->f64 { return 0.0; }", &["T003"]);
    let program = "fn f(x:f64)->f64 { return x; } fn main()->i64 { print(f(1)); return 0; }";
    let errors = rejects(program, &["T009"]);
    assert_eq!(&program[errors[0].span.start..errors[0].span.end], "1");
    assert_eq!(
        &program[errors[0].related[0].span.start..errors[0].related[0].span.end],
        "x"
    );
    for (name, good, bad) in [
        ("f64_from_i64", "1", "1.0"),
        ("f64_bits", "1.0", "1"),
        ("f64_from_bits", "1", "1.0"),
    ] {
        for args in [String::new(), format!("{good},{good}")] {
            let program = source(&format!("print({name}({args}));"));
            let errors = rejects(&program, &["T008"]);
            assert_eq!(
                &program[errors[0].span.start..errors[0].span.end],
                format!("{name}({args})")
            );
        }
        let program = source(&format!("print({name}({bad}));"));
        let errors = rejects(&program, &["T009"]);
        assert_eq!(&program[errors[0].span.start..errors[0].span.end], bad);
        rejects(&source(&format!("print({name}(true));")), &["T009"]);
        rejects(
            &source(&format!("print({name}(missing,{good}));")),
            &["T005", "T008"],
        );
    }
}

#[test]
fn float_aggregate_elements_moves_slices_and_indices_are_exactly_typed() {
    for body in [
        "let a:[f64;2]=[1.0,2];",
        "let a:[i64;2]=[1,2.0];",
        "let a:[f64;1]=[true];",
        "let a:[f64;1]=[1.0]; let b:[i64;1]=a; print(a[0]);",
        "let a:[i64;1]=[1]; let b:[f64;1]=a; print(a[0]);",
        "let a:[i64;1]=[1]; let s:&[f64]=&a;",
        "let a:[f64;1]=[1.0]; let s:&[i64]=&a;",
        "let mut a:[f64;1]=[1.0]; let s:&mut[i64]=&mut a;",
        "let mut a:[f64;1]=[1.0]; a[0]=1;",
        "let mut a:[i64;1]=[1]; a[0]=1.0;",
        "let a:[f64;1]=[1.0]; print(a[0.0]);",
        "let mut a:[f64;1]=[1.0]; a[0.0]=2.0;",
        "let a:[f64;1]=[1.0]; let s:&[f64]=&a[0.0..1];",
        "let a:[f64;1]=[1.0]; let s:&[f64]=&a[0..1.0];",
        "let a:[f64;1]=[1.0]; let s:&[f64]=&a; let x:f64=len(s);",
    ] {
        rejects(&source(body), &["T009"]);
    }
    let program = source("let a:[f64;3]=[1.0,true,2];");
    let errors = rejects(&program, &["T009", "T009"]);
    assert_eq!(&program[errors[0].span.start..errors[0].span.end], "true");
    assert_eq!(&program[errors[1].span.start..errors[1].span.end], "2");
}

#[test]
fn float_owners_and_loans_preserve_affine_safety_rules() {
    for (body, code) in [
        ("let a:[f64;1]=[1.0]; let b:[f64;1]=a; print(a[0]);", "O006"),
        ("let mut a:[f64;1]=[1.0]; let s:&[f64]=&a; a[0]=2.0;", "O007"),
        ("let mut a:[f64;1]=[1.0]; let s:&mut[f64]=&mut a; print(a[0]);", "O007"),
        ("let mut a:[f64;2]=[1.0,2.0]; let s:&mut[f64]=&mut a[0..1]; let t:&mut[f64]=&mut a[1..2];", "O007"),
        ("let a:[f64;1]=[1.0]; a[0]=2.0;", "O010"),
        ("let a:[f64;1]=[1.0]; let s:&mut[f64]=&mut a;", "O010"),
        ("let a:[f64;1]=[1.0]; let s:&[f64]=&a; s[0]=2.0;", "O010"),
        ("let a:[f64;1]=[1.0]; let s:&[f64]=&a; let t:&[f64]=s;", "O005"),
        ("let a:[f64;1]=[1.0]; let s:&[f64]=&a; let t:&[f64]=&s;", "O005"),
        ("let a:[f64;1]=[1.0]; while false { let b:[f64;1]=a; }", "O011"),
    ] {
        rejects(&source(body), &[code]);
    }
    rejects(
        &source("let mut a:[f64;1]=[1.0]; a=[2.0];"),
        &["O004", "O008"],
    );
}

#[test]
fn unsupported_numeric_extensions_have_explicit_diagnostics() {
    for expression in [
        "fast_math(1.0)",
        "fastmath(1.0)",
        "relaxed(1.0)",
        "fma(1.0,2.0,3.0)",
        "sqrt(1.0)",
        "f32_from_i64(1)",
    ] {
        let program = source(&format!("print({expression});"));
        let errors = rejects(&program, &["T007"]);
        assert_eq!(
            &program[errors[0].span.start..errors[0].span.end],
            expression
        );
    }
    for name in ["NaN", "nan", "inf", "Infinity"] {
        rejects(&source(&format!("let x:f64={name};")), &["T005"]);
    }
    rejects(&source("let x:f32=1.0;"), &["E_PARSE_TYPE"]);
    rejects_with_code(&source("let x:i64=1.0 as i64;"), "E_PARSE_EXPECTED");
    for signature in ["a:&[f64]", "a:&mut[f64]"] {
        rejects(
            &format!("fn f({signature})->i64 {{ return 0; }} fn main()->i64 {{ return 0; }}"),
            &["O003"],
        );
    }
    for ty in ["&[f64]", "&mut[f64]"] {
        rejects_with_code(
            &format!("fn f()->{ty} {{ return 0; }} fn main()->i64 {{ return 0; }}"),
            "O003",
        );
    }
    rejects_with_code(
        "fn f()->[f64;1] { return [1]; } fn main()->i64 { return 0; }",
        "T009",
    );
}

#[test]
fn f64_resource_limits_preserve_the_existing_array_local_and_node_budgets() {
    for length in [0, 33] {
        rejects(&source(&format!("let a:[f64;{length}]=[0.0];")), &["O001"]);
    }
    let values = vec!["0.0"; 32].join(",");
    let mut arrays = String::new();
    for n in 0..8 {
        write!(arrays, "let a{n}:[f64;32]=[{values}];").unwrap();
    }
    axeon::compile(&source(&arrays)).unwrap();
    rejects(
        &source(&format!("{arrays} let extra:[f64;1]=[0.0];")),
        &["O002"],
    );
    let mut locals = String::new();
    for n in 0..256 {
        write!(locals, "let x{n}:f64=0.0;").unwrap();
    }
    axeon::compile(&source(&locals)).unwrap();
    rejects(&source(&format!("{locals} let extra:f64=0.0;")), &["T015"]);
    let prints = "print(0.0);".repeat(1022);
    axeon::compile(&format!("fn main()->i64 {{ {prints} return -1; }}")).unwrap();
    rejects(
        &format!("fn main()->i64 {{ {prints} return ~-1; }}"),
        &["T016"],
    );
}

#[test]
fn f64_nesting_source_and_token_limits_keep_the_same_boundaries() {
    for (open, close) in [("(", ")"), ("-", "")] {
        let accepted = format!("{}0.0{}", open.repeat(62), close.repeat(62));
        axeon::compile(&source(&format!("print({accepted});"))).unwrap();
        let rejected = format!("{}0.0{}", open.repeat(63), close.repeat(63));
        rejects(
            &source(&format!("print({rejected});")),
            &["E_NESTING_LIMIT"],
        );
    }
    let mut intrinsic = "0".to_owned();
    for _ in 0..31 {
        intrinsic = format!("f64_bits(f64_from_bits({intrinsic}))");
    }
    axeon::compile(&source(&format!("print({intrinsic});"))).unwrap();
    rejects(
        &source(&format!("print(f64_from_bits({intrinsic}));")),
        &["E_NESTING_LIMIT"],
    );
    let mut program = source("print(0.0);");
    program.push_str(&" ".repeat(1_048_576 - program.len()));
    axeon::compile(&program).unwrap();
    program.push(' ');
    rejects(&program, &["E_SOURCE_LIMIT"]);
    let mut program = source(&"print(0.0);".repeat(178));
    for n in 0..99 {
        program.push_str(&format!(
            "fn numeric_{n}()->i64 {{ {} return 0; }}",
            "print(0.0);".repeat(198)
        ));
    }
    axeon::compile(&program).unwrap();
    program.push(';');
    rejects(&program, &["E_TOKEN_LIMIT"]);
}
