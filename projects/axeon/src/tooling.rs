//! Deterministic source formatting and offline diagnostic explanations.
//! Formatting preserves every token's exact spelling and every line comment.
use crate::diagnostic::{Diagnostic, Span};
use crate::lexer::{lex, TokenKind};

pub const MAX_FORMAT_BYTES: usize = crate::lexer::MAX_SOURCE_BYTES;

pub fn format_source(source: &str) -> Result<String, Vec<Diagnostic>> {
    // Never turn a malformed input into an apparently repaired program.
    crate::parser::validate_syntax(source)?;
    let tokens = lex(source)?;
    let mut output = String::new();
    let mut indent = 0usize;
    let mut brackets = 0usize;
    let mut parens = 0usize;
    let mut previous_end = 0;
    let mut previous = "";
    for (index, token) in tokens.iter().enumerate() {
        let gap = &source[previous_end..token.span.start];
        for comment in comments(gap) {
            newline(&mut output, token.span)?;
            append(&mut output, &"    ".repeat(indent), token.span)?;
            append(&mut output, comment, token.span)?;
            append(&mut output, "\n", token.span)?;
        }
        if token.kind == TokenKind::Eof {
            break;
        }
        let spelling = &source[token.span.start..token.span.end];
        if spelling == "}" {
            indent = indent.saturating_sub(1);
            newline(&mut output, token.span)?;
        }
        if output.is_empty() || output.ends_with('\n') {
            append(&mut output, &"    ".repeat(indent), token.span)?;
        } else if !(matches!(spelling, ")" | "]" | "," | ";" | ":" | "::" | ".")
            || matches!(previous, "(" | "[" | "::" | ".")
            || matches!(spelling, "(" | "[") && callable(previous))
        {
            append(&mut output, " ", token.span)?;
        }
        append(&mut output, spelling, token.span)?;
        match spelling {
            "{" => {
                indent += 1;
                newline(&mut output, token.span)?;
            }
            "}" => {
                let next = tokens
                    .get(index + 1)
                    .map(|next| &source[next.span.start..next.span.end]);
                if !matches!(next, Some("else" | "catch" | ";" | "," | ")" | "]" | ".")) {
                    newline(&mut output, token.span)?;
                }
            }
            "[" => brackets += 1,
            "]" => brackets = brackets.saturating_sub(1),
            "(" => parens += 1,
            ")" => parens = parens.saturating_sub(1),
            ";" if brackets == 0 && parens == 0 => newline(&mut output, token.span)?,
            _ => {}
        }
        previous_end = token.span.end;
        previous = spelling;
    }
    newline(&mut output, Span::new(source.len(), source.len()))?;
    // This is a checked transformation, including future token additions.
    // Compare spellings as well as token values: no numeric literal rewriting.
    let formatted = lex(&output)?;
    if tokens.len() != formatted.len()
        || tokens.iter().zip(&formatted).any(|(old, new)| {
            old.kind != new.kind
                || source[old.span.start..old.span.end] != output[new.span.start..new.span.end]
        })
    {
        return Err(vec![Diagnostic::new(
            "E_FORMAT_INVARIANT",
            Span::default(),
            "formatter changed the token stream; no output was published",
        )]);
    }
    Ok(output)
}

fn comments(gap: &str) -> impl Iterator<Item = &str> {
    gap.lines().filter_map(|line| {
        let text = line.trim_start();
        text.starts_with("//").then_some(text)
    })
}

fn append(output: &mut String, text: &str, span: Span) -> Result<(), Vec<Diagnostic>> {
    if text.len() > MAX_FORMAT_BYTES.saturating_sub(output.len()) {
        return Err(vec![Diagnostic::new(
            "E_FORMAT_LIMIT",
            span,
            "formatted source exceeds the 1 MiB source limit",
        )]);
    }
    output.push_str(text);
    Ok(())
}

fn newline(output: &mut String, span: Span) -> Result<(), Vec<Diagnostic>> {
    if !output.is_empty() && !output.ends_with('\n') {
        append(output, "\n", span)?;
    }
    Ok(())
}

fn callable(previous: &str) -> bool {
    previous
        .as_bytes()
        .last()
        .is_some_and(|byte| byte.is_ascii_alphanumeric() || *byte == b'_')
        && !matches!(previous, "if" | "while" | "return" | "let" | "mut")
}

/// Stable codes are searched exactly; unknown codes do not get invented advice.
pub fn explain(code: &str) -> Option<&'static str> {
    include_str!("../docs/diagnostic-explanations.tsv")
        .lines()
        .find_map(|line| {
            let (key, explanation) = line.split_once('\t')?;
            (key == code).then_some(explanation)
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dense_comments_and_final_newline_obey_output_budget_before_growth() {
        let source = format!(
            "fn main()->i64{{{}{}{}return 0;}}",
            "{".repeat(50),
            "//\n".repeat(300_000),
            "}".repeat(50)
        );
        assert!(source.len() < crate::lexer::MAX_SOURCE_BYTES);
        let errors = format_source(&source).unwrap_err();
        assert_eq!(errors[0].code, "E_FORMAT_LIMIT");
        let prefix = format_source("fn main()->i64{return 0;}").unwrap();
        let exact = format!(
            "{prefix}//{}\n",
            "x".repeat(MAX_FORMAT_BYTES - prefix.len() - 3)
        );
        assert_eq!(exact.len(), MAX_FORMAT_BYTES);
        assert_eq!(format_source(&exact).unwrap(), exact);
        let over = format!(
            "{prefix}//{}",
            "x".repeat(MAX_FORMAT_BYTES - prefix.len() - 2)
        );
        assert_eq!(over.len(), MAX_FORMAT_BYTES);
        assert_eq!(format_source(&over).unwrap_err()[0].code, "E_FORMAT_LIMIT");
    }

    #[test]
    fn formatting_is_idempotent_and_keeps_comments_and_exact_literals() {
        let source = "// π\r\nfn main()->i64{let a:[f64;2]=[1.2345678901234567e+2,-0.0]; // value\r\nprint(f64_bits(a[0]));return 0;// tail\r\n}// end";
        let formatted = format_source(source).unwrap();
        assert_eq!(format_source(&formatted).unwrap(), formatted);
        assert!(formatted.contains("1.2345678901234567e+2"));
        assert_eq!(
            comments(&formatted).collect::<Vec<_>>(),
            vec!["// π", "// value", "// tail", "// end"]
        );
        let before = crate::interpreter::run(&crate::compile(source).unwrap(), 1000);
        let after = crate::interpreter::run(&crate::compile(&formatted).unwrap(), 1000);
        assert_eq!(before, after);
    }

    #[test]
    fn comments_cannot_merge_tokens_or_hide_following_operations() {
        let source = "fn main() -> i64 {return 1 / // division\n 1 + - // sign\n 2;}";
        let formatted = format_source(source).unwrap();
        assert_eq!(
            crate::interpreter::run(&crate::compile(source).unwrap(), 100),
            crate::interpreter::run(&crate::compile(&formatted).unwrap(), 100)
        );
        assert_eq!(format_source(&formatted).unwrap(), formatted);
    }

    #[test]
    fn parser_limits_and_malformed_inputs_are_not_repaired() {
        for source in [
            "fn main() -> i64 { return ; }",
            "/* comment */",
            "fn main() -> i64 { return 1e999; }",
        ] {
            assert!(format_source(source).is_err());
        }
        assert!(format_source(&" ".repeat(crate::lexer::MAX_SOURCE_BYTES + 1)).is_err());
        assert!(explain("O007").unwrap().contains("loan"));
        assert!(explain("not-a-code").is_none());
    }
}
