//! Independent diagnostic fixtures. Expected codes, messages, byte ranges, and
//! rendered locations are written here rather than obtained from compiler output.

use axeon::diagnostic::{Diagnostic, Span};

fn diagnostic(code: &'static str, start: usize, end: usize, message: &str) -> Diagnostic {
    Diagnostic::new(code, Span::new(start, end), message)
}

fn rejects(source: &str, expected: &[Diagnostic]) {
    // Each compilation builds fresh hash maps. Compare complete diagnostics,
    // including ordering and related spans, across repeated compilations.
    for iteration in 0..8 {
        let actual = axeon::compile(source).expect_err("negative fixture must not produce IR");
        assert_eq!(
            actual, expected,
            "iteration {iteration}, source: {source:?}"
        );
        for error in &actual {
            for span in std::iter::once(error.span).chain(error.related.iter().map(|r| r.span)) {
                assert!(span.start <= span.end && span.end <= source.len());
                assert!(source.is_char_boundary(span.start));
                assert!(source.is_char_boundary(span.end));
            }
        }
    }
}

#[test]
fn entry_points_and_duplicate_functions_have_exact_diagnostics() {
    rejects(
        "fn main() -> i64 { return 0; }\nfn main() -> i64 { return 1; }",
        &[
            diagnostic("T001", 34, 38, "function `main` is already declared")
                .with_related(Span::new(3, 7), "previous function declaration"),
        ],
    );
    rejects(
        "fn helper() -> i64 { return 0; }",
        &[diagnostic(
            "T002",
            0,
            0,
            "program requires an entry function `main() -> i64`",
        )],
    );
    for source in [
        "fn main(n: i64) -> i64 { return n; }",
        "fn main() -> bool { return true; }",
    ] {
        rejects(
            source,
            &[diagnostic(
                "T003",
                3,
                7,
                "entry function must have the signature `main() -> i64`",
            )],
        );
    }
}

#[test]
fn duplicate_bindings_include_the_original_declaration() {
    for (source, duplicate, original) in [
        (
            "fn main() -> i64 { let x: i64 = 1; let x: i64 = 2; return x; }",
            39,
            23,
        ),
        (
            "fn f(x: i64, x: i64) -> i64 { return x; }\nfn main() -> i64 { return 0; }",
            13,
            5,
        ),
        (
            "fn f(x: i64) -> i64 { let x: i64 = 0; return x; }\nfn main() -> i64 { return 0; }",
            26,
            5,
        ),
    ] {
        rejects(
            source,
            &[diagnostic(
                "T004",
                duplicate,
                duplicate + 1,
                "binding `x` is already declared in this scope",
            )
            .with_related(
                Span::new(original, original + 1),
                "previous binding declaration",
            )],
        );
    }
}

#[test]
fn undefined_names_and_binding_lifetimes_are_rejected() {
    rejects(
        "fn main() -> i64 { return missing; }",
        &[diagnostic("T005", 26, 33, "unknown variable `missing`")],
    );
    rejects(
        "fn main() -> i64 { missing = 1; return 0; }",
        &[diagnostic("T005", 19, 26, "unknown variable `missing`")],
    );
    rejects(
        "fn main() -> i64 { let x: i64 = x; return x; }",
        &[diagnostic("T005", 32, 33, "unknown variable `x`")],
    );
    rejects(
        "fn main() -> i64 { { let x: i64 = 1; } return x; }",
        &[diagnostic("T005", 46, 47, "unknown variable `x`")],
    );
    rejects(
        "fn main() -> i64 { let x: i64 = 1; x = 2; return x; }",
        &[
            diagnostic("T006", 35, 36, "cannot assign to immutable binding `x`")
                .with_related(Span::new(23, 24), "immutable binding declared here"),
        ],
    );
}

#[test]
fn call_errors_cover_the_whole_call_and_link_to_its_declaration() {
    rejects(
        "fn main() -> i64 { return missing(1); }",
        &[diagnostic("T007", 26, 36, "unknown function `missing`")],
    );
    rejects(
        "fn f(n: i64) -> i64 { return n; }\nfn main() -> i64 { return f(); }",
        &[
            diagnostic("T008", 60, 63, "function `f` expects 1 arguments, found 0")
                .with_related(Span::new(3, 4), "function declared here"),
        ],
    );
    rejects(
        "fn f(n: i64) -> i64 { return n; }\nfn main() -> i64 { return f(true); }",
        &[
            diagnostic("T009", 62, 66, "argument requires i64, found bool")
                .with_related(Span::new(5, 6), "required type declared here"),
        ],
    );
}

#[test]
fn statement_type_errors_preserve_context_and_related_spans() {
    let cases = [
        (
            "fn main() -> i64 { let x: i64 = true; return 0; }",
            diagnostic("T009", 32, 36, "initializer requires i64, found bool")
                .with_related(Span::new(23, 24), "required type declared here"),
        ),
        (
            "fn main() -> i64 { let mut x: bool = false; x = 1; return 0; }",
            diagnostic("T009", 48, 49, "assignment requires bool, found i64")
                .with_related(Span::new(27, 28), "required type declared here"),
        ),
        (
            "fn main() -> i64 { return true; }",
            diagnostic("T009", 26, 30, "return value requires i64, found bool")
                .with_related(Span::new(3, 7), "required type declared here"),
        ),
        (
            "fn main() -> i64 { if 1 { return 1; } return 0; }",
            diagnostic("T009", 22, 23, "if condition requires bool, found i64"),
        ),
        (
            "fn main() -> i64 { while 1 {} return 0; }",
            diagnostic("T009", 25, 26, "while condition requires bool, found i64"),
        ),
    ];
    for (source, expected) in cases {
        rejects(source, &[expected]);
    }
}

#[test]
fn operand_errors_are_reported_independently() {
    rejects(
        "fn main() -> i64 { print(true + false); return 0; }",
        &[
            diagnostic(
                "T009",
                25,
                29,
                "left operand requires i64 or f64, found bool",
            ),
            diagnostic(
                "T009",
                32,
                37,
                "right operand requires i64 or f64, found bool",
            ),
        ],
    );
    rejects(
        "fn main() -> i64 { print(true == 1); return 0; }",
        &[diagnostic(
            "T009",
            33,
            34,
            "equality operand requires bool, found i64",
        )],
    );
    rejects(
        "fn main() -> i64 { print(!1); return 0; }",
        &[diagnostic(
            "T009",
            26,
            27,
            "unary operand requires bool, found i64",
        )],
    );
}

#[test]
fn control_flow_errors_are_exact() {
    for source in [
        "fn main() -> i64 { if true { return 1; } }",
        "fn main() -> i64 { while true { return 1; } }",
    ] {
        rejects(
            source,
            &[diagnostic(
                "T010",
                3,
                7,
                "function `main` may finish without returning i64",
            )],
        );
    }
    rejects(
        "fn main() -> i64 { return 0; print(1); }",
        &[diagnostic("T011", 29, 38, "unreachable statement")],
    );
    rejects(
        "fn main() -> i64 { break; }",
        &[diagnostic(
            "T012",
            19,
            25,
            "`break` requires an enclosing loop",
        )],
    );
    rejects(
        "fn main() -> i64 { continue; }",
        &[diagnostic(
            "T013",
            19,
            28,
            "`continue` requires an enclosing loop",
        )],
    );
}

#[test]
fn parser_annotation_and_delimiter_errors_have_exact_spans() {
    let cases = [
        (
            "fn main() -> i64 { let x = 1; return 0; }",
            diagnostic(
                "E_PARSE_EXPECTED",
                25,
                26,
                "expected ':' and an explicit variable type",
            ),
        ),
        (
            "fn main() { return 0; }",
            diagnostic(
                "E_PARSE_EXPECTED",
                10,
                11,
                "expected '->' and an explicit return type",
            ),
        ),
        (
            "fn main() -> str { return 0; }",
            diagnostic(
                "E_PARSE_TYPE",
                13,
                16,
                "unknown module-local record or union type `str`",
            ),
        ),
        (
            "fn main() -> i64 { return 0 }",
            diagnostic("E_PARSE_EXPECTED", 28, 29, "expected ';' after return"),
        ),
        (
            "fn main() -> i64 { print(1; return 0; }",
            diagnostic(
                "E_PARSE_EXPECTED",
                26,
                27,
                "expected ')' after the print argument",
            ),
        ),
        (
            "fn main() -> i64 { return 0;",
            diagnostic("E_PARSE_EXPECTED", 28, 28, "expected '}' to end the block"),
        ),
        (
            "fn main() -> i64 { return; }",
            diagnostic(
                "E_PARSE_EXPRESSION",
                25,
                26,
                "expected a literal, variable, call, borrow, index, len, or parenthesized expression",
            ),
        ),
        (
            "fn main() -> i64 { 1 + 2; return 0; }",
            diagnostic(
                "E_PARSE_STATEMENT",
                19,
                24,
                "only function or intrinsic calls may be used as expression statements",
            ),
        ),
        (
            "struct S {}",
            diagnostic(
                "E_PARSE_TOP_LEVEL",
                0,
                6,
            "expected a function, module import, record, or union declaration",
            ),
        ),
    ];
    for (source, expected) in cases {
        rejects(source, &[expected]);
    }
}

#[test]
fn literal_bounds_reject_both_sides_of_i64_without_rejecting_the_endpoints() {
    rejects(
        "fn main() -> i64 { return 9223372036854775808; }",
        &[diagnostic(
            "E_INTEGER_RANGE",
            26,
            45,
            "integer literal is outside the i64 range",
        )],
    );
    rejects(
        "fn main() -> i64 { return -9223372036854775809; }",
        &[diagnostic(
            "E_INTEGER_RANGE",
            27,
            46,
            "integer literal is outside the i64 range",
        )],
    );
    for source in [
        "fn main() -> i64 { return 9223372036854775807; }",
        "fn main() -> i64 { return -9223372036854775808; }",
    ] {
        axeon::compile(source).expect("the inclusive i64 endpoints are valid literals");
    }
}

#[test]
fn unsupported_lexical_forms_are_rejected_before_parsing() {
    rejects(
        "é 0xff 1_000 1.5x /* no */",
        &[
            diagnostic(
                "E_LEX_CHAR",
                0,
                2,
                "unsupported character 'é'; identifiers must be ASCII",
            ),
            diagnostic(
                "E_LEX_NUMBER",
                3,
                7,
                "expected decimal digits, digits.digits, or a decimal exponent with digits; suffixes and separators are unsupported",
            ),
            diagnostic(
                "E_LEX_NUMBER",
                8,
                13,
                "expected decimal digits, digits.digits, or a decimal exponent with digits; suffixes and separators are unsupported",
            ),
            diagnostic(
                "E_LEX_NUMBER",
                14,
                18,
                "expected decimal digits, digits.digits, or a decimal exponent with digits; suffixes and separators are unsupported",
            ),
            diagnostic(
                "E_LEX_COMMENT",
                19,
                27,
                "block comments are unsupported; use // line comments",
            ),
        ],
    );
    rejects(
        "/* unfinished 雪",
        &[diagnostic(
            "E_LEX_COMMENT",
            0,
            17,
            "block comments are unsupported; use // line comments",
        )],
    );
}

#[test]
fn recovery_reports_independent_statement_and_function_errors_in_order() {
    rejects(
        "fn main() -> i64 { let x: i64 = ; print(); return 0; }\nfn bad(n: str) -> i64 { return 0; }",
        &[
            diagnostic("E_PARSE_EXPRESSION", 32, 33, "expected a literal, variable, call, borrow, index, len, or parenthesized expression"),
            diagnostic("E_PARSE_EXPRESSION", 40, 41, "expected a literal, variable, call, borrow, index, len, or parenthesized expression"),
            diagnostic("E_PARSE_TYPE", 65, 68, "unknown module-local record or union type `str`"),
        ],
    );
}

#[test]
fn unicode_source_spans_use_bytes_and_rendered_columns_use_scalars() {
    let source = "// café 雪\n\u{2003}fn main() -> i64 {\n\u{2003}let x: i64 = 1;\n\u{a0}let x: i64 = true;\n\treturn x;\n}";
    let expected = [
        diagnostic("T009", 69, 73, "initializer requires i64, found bool")
            .with_related(Span::new(60, 61), "required type declared here"),
        diagnostic(
            "T004",
            60,
            61,
            "binding `x` is already declared in this scope",
        )
        .with_related(Span::new(42, 43), "previous binding declaration"),
    ];
    rejects(source, &expected);
    let actual = axeon::compile(source).unwrap_err();
    assert_eq!(actual[0].render(source, "unicode.ax"), "unicode.ax:4:15: error[T009]: initializer requires i64, found bool (bytes 69..73)\n  related at unicode.ax:4:6: required type declared here");
    assert_eq!(actual[1].render(source, "unicode.ax"), "unicode.ax:4:6: error[T004]: binding `x` is already declared in this scope (bytes 60..61)\n  related at unicode.ax:3:6: previous binding declaration");

    let source = "// café 雪\n\u{2003}fn main() -> i64 { return é; }";
    rejects(
        source,
        &[diagnostic(
            "E_LEX_CHAR",
            42,
            44,
            "unsupported character 'é'; identifiers must be ASCII",
        )],
    );
    assert_eq!(axeon::compile(source).unwrap_err()[0].render(source, "unicode.ax"), "unicode.ax:2:28: error[E_LEX_CHAR]: unsupported character 'é'; identifiers must be ASCII (bytes 42..44)");

    let source = "// λ\n\u{2003}fn main() -> i64 { return 0;";
    rejects(
        source,
        &[diagnostic(
            "E_PARSE_EXPECTED",
            37,
            37,
            "expected '}' to end the block",
        )],
    );
    assert_eq!(
        axeon::compile(source).unwrap_err()[0].render(source, "eof.ax"),
        "eof.ax:2:30: error[E_PARSE_EXPECTED]: expected '}' to end the block (bytes 37..37)"
    );
}

#[test]
fn diagnostics_remain_identical_across_repeated_compilations() {
    // Declaration errors precede body errors; body errors follow source order.
    // This pins the order even when diagnostics span forward and duplicate calls.
    let source = "fn main() -> i64 { return f(missing); }\nfn f(n: bool, n: i64) -> i64 { return true; }\nfn f() -> i64 { return 0; }";
    let expected = [
        diagnostic("T001", 89, 90, "function `f` is already declared")
            .with_related(Span::new(43, 44), "previous function declaration"),
        diagnostic("T005", 28, 35, "unknown variable `missing`"),
        diagnostic("T008", 26, 36, "function `f` expects 2 arguments, found 1")
            .with_related(Span::new(43, 44), "function declared here"),
        diagnostic(
            "T004",
            54,
            55,
            "binding `n` is already declared in this scope",
        )
        .with_related(Span::new(45, 46), "previous binding declaration"),
        diagnostic("T009", 78, 82, "return value requires i64, found bool")
            .with_related(Span::new(43, 44), "required type declared here"),
    ];
    let rendered = concat!(
        "stable.ax:3:4: error[T001]: function `f` is already declared (bytes 89..90)\n",
        "  related at stable.ax:2:4: previous function declaration\n",
        "stable.ax:1:29: error[T005]: unknown variable `missing` (bytes 28..35)\n",
        "stable.ax:1:27: error[T008]: function `f` expects 2 arguments, found 1 (bytes 26..36)\n",
        "  related at stable.ax:2:4: function declared here\n",
        "stable.ax:2:15: error[T004]: binding `n` is already declared in this scope (bytes 54..55)\n",
        "  related at stable.ax:2:6: previous binding declaration\n",
        "stable.ax:2:39: error[T009]: return value requires i64, found bool (bytes 78..82)\n",
        "  related at stable.ax:2:4: required type declared here",
    );
    for _ in 0..64 {
        let actual = axeon::compile(source).unwrap_err();
        assert_eq!(actual, expected);
        assert_eq!(
            actual
                .iter()
                .map(|d| d.render(source, "stable.ax"))
                .collect::<Vec<_>>()
                .join("\n"),
            rendered
        );
    }
}

#[test]
fn source_and_token_limits_accept_the_boundary_and_reject_one_more() {
    let mut source = String::from("fn main() -> i64 { return 0; }");
    source.push_str(&" ".repeat(1_048_576 - source.len()));
    axeon::compile(&source).expect("exactly 1 MiB of source must be accepted");
    source.push(' ');
    rejects(
        &source,
        &[diagnostic(
            "E_SOURCE_LIMIT",
            0,
            1_048_577,
            "source exceeds the 1048576-byte limit",
        )],
    );

    // Each function contributes 11 tokens plus 5 per print. Distribute
    // 19,780 prints across 100 functions to stay within the function node
    // budget while reaching exactly 100,000 tokens.
    let mut source = String::from("fn main() -> i64 { ");
    source.push_str(&"print(0);".repeat(178));
    source.push_str("return 0; }\n");
    for index in 0..99 {
        source.push_str(&format!("fn f_{index}() -> i64 {{ "));
        source.push_str(&"print(0);".repeat(198));
        source.push_str("return 0; }\n");
    }
    axeon::compile(&source).expect("exactly 100,000 tokens must be accepted");
    source.push('\u{2003}');
    let start = source.len();
    source.push(';');
    rejects(
        &source,
        &[diagnostic(
            "E_TOKEN_LIMIT",
            start,
            start + 1,
            "source exceeds the 100000-token limit",
        )],
    );
}

#[test]
fn syntax_nesting_limit_has_an_exact_boundary_and_span() {
    let accepted = format!(
        "fn main() -> i64 {{ return {}0{}; }}",
        "(".repeat(62),
        ")".repeat(62)
    );
    axeon::compile(&accepted).expect("64 levels including the block and expression are valid");
    let rejected = format!(
        "fn main() -> i64 {{ return {}0{}; }}",
        "(".repeat(63),
        ")".repeat(63)
    );
    rejects(
        &rejected,
        &[diagnostic(
            "E_NESTING_LIMIT",
            89,
            90,
            "syntax nesting exceeds the limit of 64",
        )],
    );
}

#[test]
fn function_limit_has_an_exact_boundary_and_span() {
    let mut source = String::from("fn main() -> i64 { return 0; }\n");
    for index in 0..1_023 {
        source.push_str(&format!("fn f_{index}() -> i64 {{ return 0; }}\n"));
    }
    axeon::compile(&source).expect("exactly 1,024 distinct functions must be accepted");
    let start = source.len();
    source.push_str("fn excess() -> i64 { return 0; }");
    rejects(
        &source,
        &[diagnostic(
            "E_FUNCTION_LIMIT",
            start,
            start + 2,
            "program exceeds the limit of 1024 functions",
        )],
    );
}

#[test]
fn parameter_and_local_limits_include_parameters_in_the_binding_budget() {
    for count in [32, 33] {
        let params = (0..count)
            .map(|index| format!("p{index}: i64"))
            .collect::<Vec<_>>()
            .join(", ");
        let source =
            format!("fn f({params}) -> i64 {{ return 0; }}\nfn main() -> i64 {{ return 0; }}");
        if count == 32 {
            axeon::compile(&source).expect("exactly 32 parameters must be accepted");
        } else {
            rejects(
                &source,
                &[diagnostic("T014", 3, 4, "function exceeds 32 parameters")],
            );
        }
    }

    for count in [255, 256] {
        let mut source = String::from("fn f(p: i64) -> i64 { ");
        for index in 0..count {
            source.push_str(&format!("let x{index}: i64 = 0;"));
        }
        source.push_str("return p; }\nfn main() -> i64 { return 0; }");
        if count == 255 {
            axeon::compile(&source).expect("255 locals plus one parameter must be accepted");
        } else {
            rejects(
                &source,
                &[diagnostic(
                    "T015",
                    3,
                    4,
                    "function exceeds 256 local bindings including parameters",
                )],
            );
        }
    }
}

#[test]
fn function_node_limit_accepts_the_boundary_and_rejects_one_more() {
    // A block (1), 1,022 print statements with literals (2,044), and a
    // return statement with unary negation and a literal (3) total 2,048.
    let prints = "print(0);".repeat(1_022);
    let accepted = format!("fn main() -> i64 {{ {prints} return -1; }}");
    axeon::compile(&accepted).expect("exactly 2,048 syntax nodes must be accepted");
    let rejected = format!("fn main() -> i64 {{ {prints} return ~-1; }}");
    rejects(
        &rejected,
        &[diagnostic(
            "T016",
            3,
            7,
            "function exceeds 2048 syntax nodes",
        )],
    );
}

#[test]
fn lexer_and_parser_diagnostic_budgets_are_bounded_and_deterministic() {
    let source = "💥".repeat(1_000);
    let expected: Vec<_> = (0..64)
        .map(|index| {
            diagnostic(
                "E_LEX_CHAR",
                index * 4,
                index * 4 + 4,
                "unsupported character '💥'; identifiers must be ASCII",
            )
        })
        .collect();
    rejects(&source, &expected);

    let source = format!(
        "fn main() -> i64 {{ {} return 0; }}",
        "print();".repeat(1_000)
    );
    let expected: Vec<_> = (0..64)
        .map(|index| {
            diagnostic(
                "E_PARSE_EXPRESSION",
                25 + index * 8,
                26 + index * 8,
                "expected a literal, variable, call, borrow, index, len, or parenthesized expression",
            )
        })
        .collect();
    rejects(&source, &expected);
}
