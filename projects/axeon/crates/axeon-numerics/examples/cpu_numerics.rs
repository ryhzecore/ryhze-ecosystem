//! A bounded forward numerical example; this is a Rust library API.
use axeon_numerics::{ops, Budget, Result, Tensor};

fn main() -> Result<()> {
    let budget = Budget::new(4096)?;
    let matrix = Tensor::from_slice(&budget, &[2, 3], &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0])?;
    let vector = Tensor::from_slice(&budget, &[3, 1], &[1.0, 0.0, -1.0])?;
    let product = ops::matmul(&budget, &matrix.view(), &vector.view())?;
    println!("matrix product shape: {:?}", product.shape());
    for index in 0..product.len() {
        println!(
            "product[{index}]: f64:0x{:016x}",
            product.get_linear(index)?.to_bits()
        );
    }

    let logits = Tensor::from_slice(&budget, &[3], &[1000.0, 1001.0, 999.0])?;
    let probabilities = ops::softmax(&budget, &logits.view())?;
    for index in 0..probabilities.len() {
        println!(
            "probability[{index}]: f64:0x{:016x}",
            probabilities.get_linear(index)?.to_bits()
        );
    }
    println!(
        "logsumexp: f64:0x{:016x}",
        ops::logsumexp(&logits.view())?.to_bits()
    );
    println!("live requested payload bytes: {}", budget.used_bytes());
    Ok(())
}
