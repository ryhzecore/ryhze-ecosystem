//! Stable, byte-oriented source diagnostics.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SourceId(pub usize);

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Span {
    pub source: SourceId,
    pub start: usize,
    pub end: usize,
}
impl Span {
    pub fn new(start: usize, end: usize) -> Self {
        Self::in_source(SourceId(0), start, end)
    }
    pub fn in_source(source: SourceId, start: usize, end: usize) -> Self {
        Self { source, start, end }
    }
    pub fn join(self, other: Self) -> Self {
        assert_eq!(
            self.source, other.source,
            "cannot join spans from different sources"
        );
        Self {
            source: self.source,
            start: self.start.min(other.start),
            end: self.end.max(other.end),
        }
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Related {
    pub span: Span,
    pub message: String,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Diagnostic {
    pub code: &'static str,
    pub message: String,
    pub span: Span,
    pub related: Vec<Related>,
}
impl Diagnostic {
    pub fn new(code: &'static str, span: Span, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
            span,
            related: Vec::new(),
        }
    }
    pub fn with_related(mut self, span: Span, message: impl Into<String>) -> Self {
        self.related.push(Related {
            span,
            message: message.into(),
        });
        self
    }
    pub fn render(&self, source: &str, path: &str) -> String {
        let (line, column) = line_col(source, self.span.start);
        let mut out = format!(
            "{path}:{line}:{column}: error[{}]: {} (bytes {}..{})",
            self.code, self.message, self.span.start, self.span.end
        );
        for related in &self.related {
            let (l, c) = line_col(source, related.span.start);
            out.push_str(&format!(
                "\n  related at {path}:{l}:{c}: {}",
                related.message
            ));
        }
        out
    }
    /// Render each primary/related location against its own frozen source.
    pub fn render_modules(&self, sources: &SourceTable) -> String {
        let path = sources
            .get(self.span.source)
            .expect("valid diagnostic source")
            .logical_path
            .as_str();
        let (line, column) = sources.line_col(self.span);
        let mut out = format!(
            "{path}:{line}:{column}: error[{}]: {} (bytes {}..{})",
            self.code, self.message, self.span.start, self.span.end
        );
        for related in &self.related {
            let path = sources
                .get(related.span.source)
                .expect("valid diagnostic source")
                .logical_path
                .as_str();
            let (line, column) = sources.line_col(related.span);
            out.push_str(&format!(
                "\n  related at {path}:{line}:{column}: {}",
                related.message
            ));
        }
        out
    }
}
/// One-based line and Unicode scalar column; spans are half-open byte ranges.
pub fn line_col(source: &str, offset: usize) -> (usize, usize) {
    let mut end = offset.min(source.len());
    while !source.is_char_boundary(end) {
        end -= 1;
    }
    let bytes = &source.as_bytes()[..end];
    let line = bytes.iter().filter(|&&b| b == b'\n').count() + 1;
    let start = bytes.iter().rposition(|&b| b == b'\n').map_or(0, |p| p + 1);
    let column = source[start..end].chars().count() + 1;
    (line, column)
}

/// Indexed locations for repeated backend queries, O(source bytes) construction
/// and O(log(source bytes)) lookup, including single-line Unicode sources.
#[derive(Debug, Clone)]
pub struct SourceMap {
    boundaries: Vec<usize>,
    lines: Vec<(usize, usize)>,
}
impl SourceMap {
    pub fn new(source: &str) -> Self {
        let mut boundaries = Vec::new();
        let mut lines = vec![(0, 0)];
        for (index, (offset, ch)) in source.char_indices().enumerate() {
            boundaries.push(offset);
            if ch == '\n' {
                lines.push((offset + 1, index + 1));
            }
        }
        boundaries.push(source.len());
        Self { boundaries, lines }
    }
    pub fn line_col(&self, offset: usize) -> (usize, usize) {
        let scalar = self
            .boundaries
            .partition_point(|&b| b <= offset)
            .saturating_sub(1);
        let byte = self.boundaries[scalar];
        let line = self
            .lines
            .partition_point(|&(b, _)| b <= byte)
            .saturating_sub(1);
        (line + 1, scalar - self.lines[line].1 + 1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn indexed_locations_match_unicode_scalar_locations_at_all_bytes() {
        for source in ["", "ascii\nnext\n", "雪🙂\tß\r\nlast", "\n\n"] {
            let map = SourceMap::new(source);
            for i in 0..source.len() + 3 {
                assert_eq!(map.line_col(i), line_col(source, i));
            }
        }
    }
}

/// Frozen project inputs. Source IDs index this table; logical paths never contain
/// machine-specific project prefixes. Legacy string compilation uses SourceId(0).
#[derive(Debug, Clone)]
pub struct SourceFile {
    pub logical_path: String,
    pub text: String,
    pub map: SourceMap,
}
#[derive(Debug, Clone, Default)]
pub struct SourceTable {
    files: Vec<SourceFile>,
}
impl SourceTable {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn push(&mut self, logical_path: impl Into<String>, text: impl Into<String>) -> SourceId {
        let text = text.into();
        let map = SourceMap::new(&text);
        let id = SourceId(self.files.len());
        self.files.push(SourceFile {
            logical_path: logical_path.into(),
            text,
            map,
        });
        id
    }
    pub fn get(&self, id: SourceId) -> Option<&SourceFile> {
        self.files.get(id.0)
    }
    pub fn files(&self) -> &[SourceFile] {
        &self.files
    }
    pub fn line_col(&self, span: Span) -> (usize, usize) {
        self.get(span.source)
            .expect("valid source id")
            .map
            .line_col(span.start)
    }
}
