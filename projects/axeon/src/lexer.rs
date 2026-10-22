//! Bounded lexer. All source locations are half-open UTF-8 byte ranges.
use crate::ast::{FloatIntrinsic, TensorIntrinsic};
use crate::diagnostic::{Diagnostic, SourceId, Span};

pub const MAX_SOURCE_BYTES: usize = 1_048_576;
pub const MAX_TOKENS: usize = 100_000;
pub(crate) const MAX_DIAGNOSTICS: usize = 64;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum TokenKind {
    Ident(String),
    Integer(String),
    Float(u64),
    Intrinsic(FloatIntrinsic),
    TensorIntrinsic(TensorIntrinsic),
    Tensor,
    TensorError,
    TensorScope,
    Try,
    Catch,
    Fn,
    Let,
    Mut,
    I64,
    F64,
    Bool,
    True,
    False,
    Print,
    Len,
    Return,
    If,
    Else,
    While,
    Break,
    Continue,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    DotDot,
    Dot,
    Colon,
    DoubleColon,
    Comma,
    Semicolon,
    Arrow,
    FatArrow,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Shl,
    Shr,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Bang,
    AndAnd,
    OrOr,
    Eq,
    EqEq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Eof,
}

#[derive(Debug, Clone)]
pub(crate) struct Token {
    pub kind: TokenKind,
    pub span: Span,
}

pub(crate) fn lex(source: &str) -> Result<Vec<Token>, Vec<Diagnostic>> {
    if source.len() > MAX_SOURCE_BYTES {
        return Err(vec![Diagnostic::new(
            "E_SOURCE_LIMIT",
            Span::new(0, source.len()),
            format!("source exceeds the {MAX_SOURCE_BYTES}-byte limit"),
        )]);
    }
    let bytes = source.as_bytes();
    let mut tokens = Vec::new();
    let mut diagnostics = Vec::new();
    let mut pos = 0;
    while pos < bytes.len() && diagnostics.len() < MAX_DIAGNOSTICS {
        let ch = source[pos..]
            .chars()
            .next()
            .expect("position is a character boundary");
        if ch.is_whitespace() {
            pos += ch.len_utf8();
            continue;
        }
        if source[pos..].starts_with("//") {
            pos += 2;
            while pos < bytes.len() && bytes[pos] != b'\n' {
                pos += 1;
            }
            continue;
        }
        if tokens.len() == MAX_TOKENS {
            diagnostics.push(Diagnostic::new(
                "E_TOKEN_LIMIT",
                Span::new(pos, pos + ch.len_utf8()),
                format!("source exceeds the {MAX_TOKENS}-token limit"),
            ));
            break;
        }
        let start = pos;
        let kind = if bytes[pos].is_ascii_alphabetic() || bytes[pos] == b'_' {
            pos += 1;
            while pos < bytes.len() && (bytes[pos].is_ascii_alphanumeric() || bytes[pos] == b'_') {
                pos += 1;
            }
            match &source[start..pos] {
                "fn" => TokenKind::Fn,
                "let" => TokenKind::Let,
                "mut" => TokenKind::Mut,
                "i64" => TokenKind::I64,
                "f64" => TokenKind::F64,
                "tensor" => TokenKind::Tensor,
                "tensor_error" => TokenKind::TensorError,
                "tensor_scope" => TokenKind::TensorScope,
                "try" => TokenKind::Try,
                "catch" => TokenKind::Catch,
                "f64_from_i64" => TokenKind::Intrinsic(FloatIntrinsic::FromI64),
                "f64_bits" => TokenKind::Intrinsic(FloatIntrinsic::ToBits),
                "f64_from_bits" => TokenKind::Intrinsic(FloatIntrinsic::FromBits),
                "bool" => TokenKind::Bool,
                "true" => TokenKind::True,
                "false" => TokenKind::False,
                "print" => TokenKind::Print,
                "len" => TokenKind::Len,
                "return" => TokenKind::Return,
                "if" => TokenKind::If,
                "else" => TokenKind::Else,
                "while" => TokenKind::While,
                "break" => TokenKind::Break,
                "continue" => TokenKind::Continue,
                name => TensorIntrinsic::from_name(name).map_or_else(
                    || TokenKind::Ident(name.to_owned()),
                    TokenKind::TensorIntrinsic,
                ),
            }
        } else if bytes[pos].is_ascii_digit() {
            match number(source, &mut pos) {
                Ok(kind) => kind,
                Err(error) => {
                    diagnostics.push(error);
                    continue;
                }
            }
        } else {
            let pair = bytes.get(pos..pos + 2);
            let double = match pair {
                Some(b"::") => Some(TokenKind::DoubleColon),
                Some(b"->") => Some(TokenKind::Arrow),
                Some(b"..") => Some(TokenKind::DotDot),
                Some(b"=>") => Some(TokenKind::FatArrow),
                Some(b"<<") => Some(TokenKind::Shl),
                Some(b">>") => Some(TokenKind::Shr),
                Some(b"&&") => Some(TokenKind::AndAnd),
                Some(b"||") => Some(TokenKind::OrOr),
                Some(b"==") => Some(TokenKind::EqEq),
                Some(b"!=") => Some(TokenKind::Ne),
                Some(b"<=") => Some(TokenKind::Le),
                Some(b">=") => Some(TokenKind::Ge),
                _ => None,
            };
            if let Some(kind) = double {
                pos += 2;
                kind
            } else {
                pos += ch.len_utf8();
                match ch {
                    '(' => TokenKind::LParen,
                    ')' => TokenKind::RParen,
                    '{' => TokenKind::LBrace,
                    '}' => TokenKind::RBrace,
                    '[' => TokenKind::LBracket,
                    ']' => TokenKind::RBracket,
                    '.' if !bytes.get(pos).is_some_and(u8::is_ascii_digit) => TokenKind::Dot,
                    ':' => TokenKind::Colon,
                    ',' => TokenKind::Comma,
                    ';' => TokenKind::Semicolon,
                    '+' => TokenKind::Plus,
                    '-' => TokenKind::Minus,
                    '*' => TokenKind::Star,
                    '/' if bytes.get(pos) == Some(&b'*') => {
                        pos += 1;
                        // Skip an unsupported comment as one unit, keeping recovery useful.
                        if let Some(end) = source[pos..].find("*/") {
                            pos += end + 2;
                        } else {
                            pos = bytes.len();
                        }
                        diagnostics.push(Diagnostic::new(
                            "E_LEX_COMMENT",
                            Span::new(start, pos),
                            "block comments are unsupported; use // line comments",
                        ));
                        continue;
                    }
                    '/' => TokenKind::Slash,
                    '%' => TokenKind::Percent,
                    '&' => TokenKind::Amp,
                    '|' => TokenKind::Pipe,
                    '^' => TokenKind::Caret,
                    '~' => TokenKind::Tilde,
                    '!' => TokenKind::Bang,
                    '=' => TokenKind::Eq,
                    '<' => TokenKind::Lt,
                    '>' => TokenKind::Gt,
                    _ => {
                        diagnostics.push(Diagnostic::new(
                            "E_LEX_CHAR",
                            Span::new(start, pos),
                            format!("unsupported character {ch:?}; identifiers must be ASCII"),
                        ));
                        continue;
                    }
                }
            }
        };
        tokens.push(Token {
            kind,
            span: Span::new(start, pos),
        });
    }
    tokens.push(Token {
        kind: TokenKind::Eof,
        span: Span::new(source.len(), source.len()),
    });
    if diagnostics.is_empty() {
        Ok(tokens)
    } else {
        Err(diagnostics)
    }
}

/// The same lexer and budgets as single-source compilation, with a file identity.
pub(crate) fn lex_in_source(source: &str, id: SourceId) -> Result<Vec<Token>, Vec<Diagnostic>> {
    match lex(source) {
        Ok(mut tokens) => {
            for token in &mut tokens {
                token.span.source = id;
            }
            Ok(tokens)
        }
        Err(mut errors) => {
            for error in &mut errors {
                error.span.source = id;
                for related in &mut error.related {
                    related.span.source = id;
                }
            }
            Err(errors)
        }
    }
}

/// Decimal floats require a fractional part with digits or a decimal exponent.
/// Range dots never belong to a numeric token. Conversion is performed once,
/// retaining IEEE bits; gradual underflow to a subnormal or +0 is permitted.
fn number(source: &str, pos: &mut usize) -> Result<TokenKind, Diagnostic> {
    let bytes = source.as_bytes();
    let start = *pos;
    while *pos < bytes.len() && bytes[*pos].is_ascii_digit() {
        *pos += 1;
    }
    let mut is_float = false;
    let mut malformed = false;
    if bytes.get(*pos) == Some(&b'.') && !source[*pos..].starts_with("..") {
        is_float = true;
        *pos += 1;
        let fractional_start = *pos;
        while *pos < bytes.len() && bytes[*pos].is_ascii_digit() {
            *pos += 1;
        }
        malformed |= *pos == fractional_start;
    }
    if matches!(bytes.get(*pos), Some(b'e' | b'E')) {
        is_float = true;
        *pos += 1;
        if matches!(bytes.get(*pos), Some(b'+' | b'-')) {
            *pos += 1;
        }
        let exponent_start = *pos;
        while *pos < bytes.len() && bytes[*pos].is_ascii_digit() {
            *pos += 1;
        }
        malformed |= *pos == exponent_start;
    }
    while *pos < bytes.len()
        && (bytes[*pos].is_ascii_alphanumeric()
            || bytes[*pos] == b'_'
            || (bytes[*pos] == b'.' && !source[*pos..].starts_with("..")))
    {
        malformed = true;
        *pos += 1;
    }
    let span = Span::new(start, *pos);
    if malformed {
        return Err(Diagnostic::new("E_LEX_NUMBER", span,
            "expected decimal digits, digits.digits, or a decimal exponent with digits; suffixes and separators are unsupported"));
    }
    let spelling = &source[start..*pos];
    if !is_float {
        return Ok(TokenKind::Integer(spelling.to_owned()));
    }
    match spelling.parse::<f64>() {
        Ok(value) if value.is_finite() => Ok(TokenKind::Float(value.to_bits())),
        Ok(_) => Err(Diagnostic::new(
            "E_FLOAT_RANGE",
            span,
            "floating-point literal exceeds the finite f64 range",
        )),
        Err(_) => Err(Diagnostic::new(
            "E_LEX_NUMBER",
            span,
            "malformed decimal floating-point literal",
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unicode_comment_and_whitespace_preserve_byte_spans() {
        let source = "// café 雪\n\u{2003}fn hello";
        let tokens = lex(source).unwrap();
        let start = source.find("fn").unwrap();
        assert_eq!(tokens[0].span, Span::new(start, start + 2));
        assert_eq!(tokens[1].span, Span::new(start + 3, source.len()));
        assert_eq!(tokens[2].span, Span::new(source.len(), source.len()));
    }

    #[test]
    fn record_field_dot_preserves_range_and_decimal_tokens() {
        let tokens = lex("record.value a[0..2] 1.25").unwrap();
        assert_eq!(tokens[0].kind, TokenKind::Ident("record".into()));
        assert_eq!(tokens[1].kind, TokenKind::Dot);
        assert_eq!(tokens[2].kind, TokenKind::Ident("value".into()));
        assert_eq!(tokens[6].kind, TokenKind::DotDot);
        assert_eq!(tokens[9].kind, TokenKind::Float(1.25f64.to_bits()));
    }

    #[test]
    fn operators_use_longest_match() {
        let got: Vec<_> = lex("-> << >> && || == != <= >= + - * / % & | ^ ~ ! = < >")
            .unwrap()
            .into_iter()
            .map(|t| t.kind)
            .collect();
        assert_eq!(
            got,
            vec![
                TokenKind::Arrow,
                TokenKind::Shl,
                TokenKind::Shr,
                TokenKind::AndAnd,
                TokenKind::OrOr,
                TokenKind::EqEq,
                TokenKind::Ne,
                TokenKind::Le,
                TokenKind::Ge,
                TokenKind::Plus,
                TokenKind::Minus,
                TokenKind::Star,
                TokenKind::Slash,
                TokenKind::Percent,
                TokenKind::Amp,
                TokenKind::Pipe,
                TokenKind::Caret,
                TokenKind::Tilde,
                TokenKind::Bang,
                TokenKind::Eq,
                TokenKind::Lt,
                TokenKind::Gt,
                TokenKind::Eof
            ]
        );
    }

    #[test]
    fn range_tokens_do_not_turn_integer_endpoints_into_floats() {
        let source = "&mut a[0..12] len(a) x & y && z";
        let tokens = lex(source).unwrap();
        assert_eq!(
            tokens.iter().map(|t| t.kind.clone()).collect::<Vec<_>>(),
            vec![
                TokenKind::Amp,
                TokenKind::Mut,
                TokenKind::Ident("a".into()),
                TokenKind::LBracket,
                TokenKind::Integer("0".into()),
                TokenKind::DotDot,
                TokenKind::Integer("12".into()),
                TokenKind::RBracket,
                TokenKind::Len,
                TokenKind::LParen,
                TokenKind::Ident("a".into()),
                TokenKind::RParen,
                TokenKind::Ident("x".into()),
                TokenKind::Amp,
                TokenKind::Ident("y".into()),
                TokenKind::AndAnd,
                TokenKind::Ident("z".into()),
                TokenKind::Eof,
            ]
        );
        assert_eq!(&source[tokens[5].span.start..tokens[5].span.end], "..");
        for unsupported in ["1.5.0", "1.foo", "1...2"] {
            assert!(lex(unsupported).is_err(), "{unsupported}");
        }
    }

    #[test]
    fn rejects_unicode_identifiers_and_unsupported_literals() {
        let errors = lex("é 0xff 1_000 1.5f64 /* no */").unwrap_err();
        assert_eq!(
            errors.iter().map(|e| e.code).collect::<Vec<_>>(),
            vec![
                "E_LEX_CHAR",
                "E_LEX_NUMBER",
                "E_LEX_NUMBER",
                "E_LEX_NUMBER",
                "E_LEX_COMMENT"
            ]
        );
        assert_eq!(errors[0].span, Span::new(0, 2));
    }

    #[test]
    fn float_literals_round_to_bits_and_allow_gradual_underflow() {
        for (spelling, expected_bits) in [
            ("0.0", 0),
            ("0e10000", 0),
            ("1.0", 1.0f64.to_bits()),
            ("0.1", 0x3fb9_9999_9999_999a),
            ("01.250", 1.25f64.to_bits()),
            ("12e2", 1200.0f64.to_bits()),
            ("12E+2", 1200.0f64.to_bits()),
            ("1.25e-2", 0.0125f64.to_bits()),
            ("1.7976931348623157e308", f64::MAX.to_bits()),
            ("2.2250738585072014e-308", f64::MIN_POSITIVE.to_bits()),
            ("5e-324", 1),
            ("1e-324", 0),
            ("1e-9999", 0),
            // Halfway between 1 and its successor: round ties to the even significand.
            (
                "1.00000000000000011102230246251565404236316680908203125",
                1.0f64.to_bits(),
            ),
        ] {
            let tokens = lex(spelling).unwrap();
            assert_eq!(
                tokens[0].kind,
                TokenKind::Float(expected_bits),
                "{spelling}"
            );
            assert_eq!(tokens[0].span, Span::new(0, spelling.len()));
        }
        assert_eq!(lex("1").unwrap()[0].kind, TokenKind::Integer("1".into()));
    }

    #[test]
    fn rejects_float_overflow_and_malformed_numbers_with_full_spans() {
        for spelling in ["1e309", "1.7976931348623159e308", "99999e99999"] {
            let errors = lex(spelling).unwrap_err();
            assert_eq!(errors[0].code, "E_FLOAT_RANGE");
            assert_eq!(errors[0].span, Span::new(0, spelling.len()));
        }
        for spelling in [
            "1.", "1.e2", "1e", "1e+", "1e-", "1.0e+", "1e2e3", "1.0f64", "1e2_0", "0x1.2", "1.2.3",
        ] {
            let errors = lex(spelling).unwrap_err();
            assert_eq!(errors[0].code, "E_LEX_NUMBER", "{spelling}");
            assert_eq!(errors[0].span, Span::new(0, spelling.len()), "{spelling}");
        }
        assert!(lex(".5").is_err());
        let huge = format!("1e{}", "9".repeat(100_000));
        assert_eq!(lex(&huge).unwrap_err()[0].code, "E_FLOAT_RANGE");
        let underflow = format!("1e-{}", "9".repeat(100_000));
        assert_eq!(lex(&underflow).unwrap()[0].kind, TokenKind::Float(0));
    }

    #[test]
    fn floats_leave_ranges_intact_and_intrinsic_names_are_reserved() {
        let got: Vec<_> = lex("1..2 1.0..2.0 1e2..3 &a[0..len(a)]")
            .unwrap()
            .into_iter()
            .map(|t| t.kind)
            .collect();
        assert_eq!(
            &got[..9],
            &[
                TokenKind::Integer("1".into()),
                TokenKind::DotDot,
                TokenKind::Integer("2".into()),
                TokenKind::Float(1.0f64.to_bits()),
                TokenKind::DotDot,
                TokenKind::Float(2.0f64.to_bits()),
                TokenKind::Float(100.0f64.to_bits()),
                TokenKind::DotDot,
                TokenKind::Integer("3".into()),
            ]
        );
        let got: Vec<_> = lex("f64 f64_from_i64 f64_bits f64_from_bits")
            .unwrap()
            .into_iter()
            .map(|t| t.kind)
            .collect();
        assert_eq!(
            got,
            vec![
                TokenKind::F64,
                TokenKind::Intrinsic(FloatIntrinsic::FromI64),
                TokenKind::Intrinsic(FloatIntrinsic::ToBits),
                TokenKind::Intrinsic(FloatIntrinsic::FromBits),
                TokenKind::Eof
            ]
        );
    }

    #[test]
    fn source_and_token_limits_are_enforced() {
        assert!(lex(&" ".repeat(MAX_SOURCE_BYTES)).is_ok());
        assert_eq!(
            lex(&" ".repeat(MAX_SOURCE_BYTES + 1)).unwrap_err()[0].code,
            "E_SOURCE_LIMIT"
        );
        assert!(lex(&";".repeat(MAX_TOKENS)).is_ok());
        assert_eq!(
            lex(&";".repeat(MAX_TOKENS + 1)).unwrap_err()[0].code,
            "E_TOKEN_LIMIT"
        );
        assert_eq!(lex(&"💥".repeat(1000)).unwrap_err().len(), MAX_DIAGNOSTICS);
    }
}
