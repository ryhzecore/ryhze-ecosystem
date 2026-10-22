//! Bounded fixture transport over the public optional Rust library API.
#![forbid(unsafe_code)]

use axeon_numerics::{ops, Budget, Tensor, View};
use std::fmt::Write as _;
use std::io::{Read, Write};

const INPUT_LIMIT: usize = 1_048_576;
const LINE_LIMIT: usize = 262_144;

enum Transform {
    Permute(Vec<usize>),
    Slice(usize, usize, usize, usize),
}

struct InputTensor {
    shape: Vec<usize>,
    values: Vec<f64>,
    transforms: Vec<Transform>,
}

struct Request {
    id: String,
    operation: String,
    a: InputTensor,
    b: Option<InputTensor>,
}

struct Tokens<'a>(std::str::SplitAsciiWhitespace<'a>);

impl<'a> Tokens<'a> {
    fn text(&mut self) -> Result<&'a str, String> {
        self.0.next().ok_or_else(|| "missing field".into())
    }

    fn integer(&mut self) -> Result<usize, String> {
        let text = self.text()?;
        if !text.bytes().all(|value| value.is_ascii_digit()) {
            return Err("invalid unsigned decimal".into());
        }
        text.parse().map_err(|_| "decimal outside usize".into())
    }

    fn tensor(&mut self) -> Result<InputTensor, String> {
        let rank = self.integer()?;
        if rank > 4 {
            return Err("transport rank limit".into());
        }
        let mut shape = Vec::with_capacity(rank);
        for _ in 0..rank {
            shape.push(self.integer()?);
        }
        let count = self.integer()?;
        if count > 4096 {
            return Err("transport element limit".into());
        }
        let mut values = Vec::with_capacity(count);
        for _ in 0..count {
            let hex = self.text()?;
            if hex.len() != 16
                || !hex
                    .bytes()
                    .all(|value| value.is_ascii_digit() || (b'a'..=b'f').contains(&value))
            {
                return Err("invalid binary64 encoding".into());
            }
            let bits = u64::from_str_radix(hex, 16).map_err(|_| "invalid hex")?;
            values.push(f64::from_bits(bits));
        }
        let count = self.integer()?;
        if count > 16 {
            return Err("transport transform limit".into());
        }
        let mut transforms = Vec::with_capacity(count);
        for _ in 0..count {
            transforms.push(match self.text()? {
                "p" => {
                    let rank = self.integer()?;
                    if rank > 4 {
                        return Err("transport permutation limit".into());
                    }
                    let mut axes = Vec::with_capacity(rank);
                    for _ in 0..rank {
                        axes.push(self.integer()?);
                    }
                    Transform::Permute(axes)
                }
                "s" => Transform::Slice(
                    self.integer()?,
                    self.integer()?,
                    self.integer()?,
                    self.integer()?,
                ),
                _ => return Err("unknown transform".into()),
            });
        }
        Ok(InputTensor {
            shape,
            values,
            transforms,
        })
    }
}

fn parse(line: &str) -> Result<Request, String> {
    if line.len() > LINE_LIMIT || !line.is_ascii() {
        return Err("transport line limit or non-ASCII input".into());
    }
    let mut tokens = Tokens(line.split_ascii_whitespace());
    if tokens.text()? != "v1" {
        return Err("unsupported transport version".into());
    }
    let id = tokens.text()?.to_owned();
    if !id
        .bytes()
        .all(|value| value.is_ascii_alphanumeric() || value == b'_')
    {
        return Err("invalid fixture identifier".into());
    }
    let operation = tokens.text()?.to_owned();
    if !matches!(
        operation.as_str(),
        "add" | "mul" | "sum" | "matmul" | "sigmoid" | "softmax" | "logsumexp"
    ) {
        return Err("unknown operation".into());
    }
    let a = tokens.tensor()?;
    let b = match tokens.integer()? {
        0 => None,
        1 => Some(tokens.tensor()?),
        _ => return Err("invalid second-input marker".into()),
    };
    if b.is_some() != matches!(operation.as_str(), "add" | "mul" | "matmul") {
        return Err("operation arity mismatch".into());
    }
    if tokens.0.next().is_some() {
        return Err("trailing fields".into());
    }
    Ok(Request {
        id,
        operation,
        a,
        b,
    })
}

fn transformed<'a>(
    mut view: View<'a>,
    transforms: &[Transform],
) -> axeon_numerics::Result<View<'a>> {
    for transform in transforms {
        view = match transform {
            Transform::Permute(axes) => view.transpose(axes)?,
            Transform::Slice(axis, start, end, step) => {
                view.axis_slice(*axis, *start, *end, *step)?
            }
        };
    }
    Ok(view)
}

fn collect(tensor: Tensor<'_>) -> axeon_numerics::Result<(Vec<usize>, Vec<u64>)> {
    let mut values = Vec::with_capacity(tensor.len());
    for index in 0..tensor.len() {
        values.push(tensor.get_linear(index)?.to_bits());
    }
    Ok((tensor.shape().to_vec(), values))
}

fn execute_with_budget(
    request: &Request,
    budget: &Budget,
) -> axeon_numerics::Result<(Vec<usize>, Vec<u64>)> {
    let a = Tensor::from_slice(budget, &request.a.shape, &request.a.values)?;
    let a = transformed(a.view(), &request.a.transforms)?;
    let b_owner = request
        .b
        .as_ref()
        .map(|input| Tensor::from_slice(budget, &input.shape, &input.values))
        .transpose()?;
    let b = match (b_owner.as_ref(), request.b.as_ref()) {
        (Some(owner), Some(input)) => Some(transformed(owner.view(), &input.transforms)?),
        _ => None,
    };
    match request.operation.as_str() {
        "add" => collect(ops::add(budget, &a, b.as_ref().unwrap())?),
        "mul" => collect(ops::mul(budget, &a, b.as_ref().unwrap())?),
        "matmul" => collect(ops::matmul(budget, &a, b.as_ref().unwrap())?),
        "sum" => Ok((vec![], vec![ops::sum(&a)?.to_bits()])),
        "sigmoid" => collect(ops::sigmoid(budget, &a)?),
        "softmax" => collect(ops::softmax(budget, &a)?),
        "logsumexp" => Ok((vec![], vec![ops::logsumexp(&a)?.to_bits()])),
        _ => unreachable!(),
    }
}

fn execute(request: &Request) -> axeon_numerics::Result<(Vec<usize>, Vec<u64>)> {
    let budget = Budget::new(32 * 1024 * 1024)?;
    let result = execute_with_budget(request, &budget);
    assert_eq!(budget.used_bytes(), 0, "fixture leaked budget accounting");
    result
}

fn run() -> Result<(), String> {
    let mut input = String::new();
    std::io::stdin()
        .take((INPUT_LIMIT + 1) as u64)
        .read_to_string(&mut input)
        .map_err(|error| error.to_string())?;
    if input.len() > INPUT_LIMIT || input.lines().count() > 512 {
        return Err("transport input limit".into());
    }
    let mut stdout = std::io::BufWriter::new(std::io::stdout().lock());
    for line in input.lines() {
        let request = parse(line)?;
        let mut response = format!("v1 {} ", request.id);
        match execute(&request) {
            Err(error) => writeln!(response, "err {error:?}").unwrap(),
            Ok((shape, values)) => {
                write!(response, "ok {}", shape.len()).unwrap();
                for extent in shape {
                    write!(response, " {extent}").unwrap();
                }
                write!(response, " {}", values.len()).unwrap();
                for bits in values {
                    write!(response, " {bits:016x}").unwrap();
                }
                response.push('\n');
            }
        }
        stdout
            .write_all(response.as_bytes())
            .map_err(|error| error.to_string())?;
    }
    stdout.flush().map_err(|error| error.to_string())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("fixture transport: {error}");
        std::process::exit(2);
    }
}
