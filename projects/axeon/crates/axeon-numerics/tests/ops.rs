use axeon_numerics::{ops, Budget, Error, Result, Tensor, CANONICAL_NAN_BITS};

fn bits(tensor: &Tensor<'_>) -> Vec<u64> {
    (0..tensor.len())
        .map(|index| tensor.get_linear(index).unwrap().to_bits())
        .collect()
}

fn expected_bits(values: &[f64]) -> Vec<u64> {
    values.iter().map(|value| value.to_bits()).collect()
}

fn expect_error<T>(result: Result<T>, expected: Error) {
    match result {
        Ok(_) => panic!("operation unexpectedly succeeded; expected {expected:?}"),
        Err(actual) => assert_eq!(actual, expected),
    }
}

#[test]
fn elementwise_operations_preserve_shape_and_canonical_observers() {
    let budget = Budget::new(4096).unwrap();
    let left = Tensor::from_slice(
        &budget,
        &[2, 3],
        &[-0.0, f64::INFINITY, 3.0, f64::NAN, -2.0, f64::MAX],
    )
    .unwrap();
    let right = Tensor::from_slice(
        &budget,
        &[2, 3],
        &[-0.0, f64::NEG_INFINITY, 4.0, 1.0, 2.0, 2.0],
    )
    .unwrap();
    let added = ops::add(&budget, &left.view(), &right.view()).unwrap();
    assert_eq!(added.shape(), &[2, 3]);
    assert_eq!(
        bits(&added),
        vec![
            (-0.0_f64).to_bits(),
            CANONICAL_NAN_BITS,
            7.0_f64.to_bits(),
            CANONICAL_NAN_BITS,
            0.0_f64.to_bits(),
            f64::MAX.to_bits(),
        ]
    );
    let multiplied = ops::mul(&budget, &left.view(), &right.view()).unwrap();
    assert_eq!(
        bits(&multiplied),
        vec![
            0.0_f64.to_bits(),
            f64::NEG_INFINITY.to_bits(),
            12.0_f64.to_bits(),
            CANONICAL_NAN_BITS,
            (-4.0_f64).to_bits(),
            f64::INFINITY.to_bits(),
        ]
    );
    assert_eq!(left.get_linear(0).unwrap().to_bits(), (-0.0_f64).to_bits());
}

#[test]
fn elementwise_shapes_must_match_even_with_equal_element_counts() {
    let inputs = Budget::new(1024).unwrap();
    let full = Budget::new(0).unwrap();
    let vector = Tensor::zeros(&inputs, &[4]).unwrap();
    let matrix = Tensor::zeros(&inputs, &[2, 2]).unwrap();
    expect_error(
        ops::add(&full, &vector.view(), &matrix.view()),
        Error::ShapeMismatch,
    );
    expect_error(
        ops::mul(&full, &vector.view(), &matrix.view()),
        Error::ShapeMismatch,
    );
    assert_eq!(full.used_bytes(), 0);
}

#[test]
fn scalar_and_empty_elementwise_outputs_obey_explicit_budget() {
    let inputs = Budget::new(1024).unwrap();
    let output_budget = Budget::new(8).unwrap();
    let scalar = Tensor::from_slice(&inputs, &[], &[3.0]).unwrap();
    let output = ops::mul(&output_budget, &scalar.view(), &scalar.view()).unwrap();
    assert_eq!(output.shape(), &[]);
    assert_eq!(output.get_linear(0).unwrap(), 9.0);
    assert_eq!(output_budget.used_bytes(), 8);
    expect_error(
        ops::add(&output_budget, &scalar.view(), &scalar.view()),
        Error::BudgetLimit,
    );
    assert_eq!(output_budget.used_bytes(), 8);
    drop(output);
    assert_eq!(output_budget.used_bytes(), 0);
    let empty = Tensor::zeros(&inputs, &[3, 0, 2]).unwrap();
    let no_payload = Budget::new(0).unwrap();
    let empty_output = ops::add(&no_payload, &empty.view(), &empty.view()).unwrap();
    assert_eq!(empty_output.shape(), &[3, 0, 2]);
    assert!(empty_output.is_empty());
    assert_eq!(no_payload.used_bytes(), 0);
}

#[test]
fn sum_uses_logical_order_and_positive_zero_identity() {
    let budget = Budget::new(1024).unwrap();
    let ordered = Tensor::from_slice(&budget, &[3], &[1e16, 1.0, -1e16]).unwrap();
    assert_eq!(ops::sum(&ordered.view()).unwrap().to_bits(), 0);
    let source = Tensor::from_slice(&budget, &[2, 2], &[1e16, -1e16, 1.0, 0.0]).unwrap();
    assert_eq!(ops::sum(&source.view()).unwrap(), 1.0);
    let transposed = source.view().transpose(&[1, 0]).unwrap();
    assert_eq!(ops::sum(&transposed).unwrap().to_bits(), 0);
    for values in [&[][..], &[-0.0, -0.0][..]] {
        let tensor = Tensor::from_slice(&budget, &[values.len()], values).unwrap();
        assert_eq!(ops::sum(&tensor.view()).unwrap().to_bits(), 0);
    }
    let infinities =
        Tensor::from_slice(&budget, &[2], &[f64::INFINITY, f64::NEG_INFINITY]).unwrap();
    assert_eq!(
        ops::sum(&infinities.view()).unwrap().to_bits(),
        CANONICAL_NAN_BITS
    );
}

#[test]
fn kernels_read_transposed_sliced_and_reborrowed_views_in_logical_order() {
    let budget = Budget::new(4096).unwrap();
    let mut source = Tensor::from_slice(&budget, &[2, 3], &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).unwrap();
    let transformed = source
        .view()
        .transpose(&[1, 0])
        .unwrap()
        .axis_slice(0, 0, 3, 2)
        .unwrap();
    let doubled = ops::add(&budget, &transformed, &transformed).unwrap();
    assert_eq!(doubled.shape(), &[2, 2]);
    assert_eq!(bits(&doubled), expected_bits(&[2.0, 8.0, 6.0, 12.0]));
    let mut mutable = source.view_mut().transpose(&[1, 0]).unwrap();
    mutable.set_linear(0, 10.0).unwrap();
    assert_eq!(ops::sum(&mutable.view()).unwrap(), 30.0);
}

#[test]
fn matrix_product_checks_reduction_order_and_separate_rounding() {
    let budget = Budget::new(4096).unwrap();
    let left = Tensor::from_slice(&budget, &[1, 3], &[1e16, 1.0, -1e16]).unwrap();
    let right = Tensor::from_slice(&budget, &[3, 1], &[1.0, 1.0, 1.0]).unwrap();
    let ordered = ops::matmul(&budget, &left.view(), &right.view()).unwrap();
    assert_eq!(ordered.get_linear(0).unwrap().to_bits(), 0);

    let a = f64::from_bits(0x3ff0_0000_0000_0001);
    let b = f64::from_bits(0x3fef_ffff_ffff_fffe);
    assert_ne!(a.mul_add(b, -1.0), 0.0, "fixture must distinguish FMA");
    let left = Tensor::from_slice(&budget, &[1, 2], &[-1.0, a]).unwrap();
    let right = Tensor::from_slice(&budget, &[2, 1], &[1.0, b]).unwrap();
    let separate = ops::matmul(&budget, &left.view(), &right.view()).unwrap();
    assert_eq!(separate.get_linear(0).unwrap().to_bits(), 0);
}

#[test]
fn matrix_product_supports_strided_operands_and_canonical_nan() {
    let budget = Budget::new(4096).unwrap();
    let source = Tensor::from_slice(&budget, &[2, 3], &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).unwrap();
    let right = source.view().transpose(&[1, 0]).unwrap();
    let product = ops::matmul(&budget, &source.view(), &right).unwrap();
    assert_eq!(product.shape(), &[2, 2]);
    assert_eq!(bits(&product), expected_bits(&[14.0, 32.0, 32.0, 77.0]));
    let infinity = Tensor::from_slice(&budget, &[1, 1], &[f64::INFINITY]).unwrap();
    let zero = Tensor::zeros(&budget, &[1, 1]).unwrap();
    let nan = ops::matmul(&budget, &infinity.view(), &zero.view()).unwrap();
    assert_eq!(nan.get_linear(0).unwrap().to_bits(), CANONICAL_NAN_BITS);
}

#[test]
fn empty_matrix_axes_and_empty_reductions_have_defined_outputs() {
    let inputs = Budget::new(1024).unwrap();
    let outputs = Budget::new(128).unwrap();
    let left = Tensor::zeros(&inputs, &[3, 0]).unwrap();
    let right = Tensor::zeros(&inputs, &[0, 2]).unwrap();
    let result = ops::matmul(&outputs, &left.view(), &right.view()).unwrap();
    assert_eq!(result.shape(), &[3, 2]);
    assert_eq!(bits(&result), vec![0; 6]);
    let left = Tensor::zeros(&inputs, &[0, 3]).unwrap();
    let right = Tensor::zeros(&inputs, &[3, 2]).unwrap();
    let result = ops::matmul(&outputs, &left.view(), &right.view()).unwrap();
    assert_eq!(result.shape(), &[0, 2]);
    assert!(result.is_empty());
}

#[test]
fn matrix_shape_and_work_errors_precede_output_allocation() {
    let inputs = Budget::new(1 << 20).unwrap();
    let no_output_space = Budget::new(0).unwrap();
    let vector = Tensor::zeros(&inputs, &[2]).unwrap();
    let matrix = Tensor::zeros(&inputs, &[2, 2]).unwrap();
    expect_error(
        ops::matmul(&no_output_space, &vector.view(), &matrix.view()),
        Error::RankMismatch,
    );
    let incompatible = Tensor::zeros(&inputs, &[3, 1]).unwrap();
    expect_error(
        ops::matmul(&no_output_space, &matrix.view(), &incompatible.view()),
        Error::ShapeMismatch,
    );
    let left = Tensor::zeros(&inputs, &[256, 16]).unwrap();
    let right = Tensor::zeros(&inputs, &[16, 256]).unwrap();
    expect_error(
        ops::matmul(&no_output_space, &left.view(), &right.view()),
        Error::WorkLimit,
    );
    let left = Tensor::zeros(&inputs, &[65_536, 0]).unwrap();
    let right = Tensor::zeros(&inputs, &[0, 65_536]).unwrap();
    expect_error(
        ops::matmul(&no_output_space, &left.view(), &right.view()),
        Error::ElementLimit,
    );
    assert_eq!(no_output_space.used_bytes(), 0);
}

#[test]
fn matrix_work_limit_includes_output_elements_and_accepts_exact_boundary() {
    assert_eq!(ops::MAX_MATMUL_WORK, 256 * 256 * (15 + 1));
    let budget = Budget::new(1 << 20).unwrap();
    let left = Tensor::zeros(&budget, &[256, 15]).unwrap();
    let right = Tensor::zeros(&budget, &[15, 256]).unwrap();
    let result = ops::matmul(&budget, &left.view(), &right.view()).unwrap();
    assert_eq!(result.len(), 65_536);
    assert!(bits(&result).iter().all(|bits| *bits == 0));
}

#[test]
fn sigmoid_uses_stable_branches_and_logical_views() {
    let budget = Budget::new(1024).unwrap();
    let input = Tensor::from_slice(&budget, &[2, 2], &[0.0, 1000.0, -1000.0, -0.0]).unwrap();
    let view = input.view().transpose(&[1, 0]).unwrap();
    let result = ops::sigmoid(&budget, &view).unwrap();
    assert_eq!(result.shape(), &[2, 2]);
    assert_eq!(bits(&result), expected_bits(&[0.5, 0.0, 1.0, 0.5]));
    let empty = Tensor::zeros(&budget, &[0, 3]).unwrap();
    assert!(ops::sigmoid(&budget, &empty.view()).unwrap().is_empty());
    let scalar = Tensor::from_slice(&budget, &[], &[0.0]).unwrap();
    assert_eq!(ops::sigmoid(&budget, &scalar.view()).unwrap().shape(), &[]);
}

#[test]
fn softmax_handles_extreme_offsets_and_strided_vectors() {
    let budget = Budget::new(4096).unwrap();
    for values in [[1000.0, 1000.0], [-1000.0, -1000.0], [0.0, -0.0]] {
        let input = Tensor::from_slice(&budget, &[2], &values).unwrap();
        let result = ops::softmax(&budget, &input.view()).unwrap();
        assert_eq!(bits(&result), expected_bits(&[0.5, 0.5]));
    }
    let extremes = Tensor::from_slice(&budget, &[2], &[-f64::MAX, f64::MAX]).unwrap();
    assert_eq!(
        bits(&ops::softmax(&budget, &extremes.view()).unwrap()),
        expected_bits(&[0.0, 1.0])
    );
    assert_eq!(ops::logsumexp(&extremes.view()).unwrap(), f64::MAX);
    let source =
        Tensor::from_slice(&budget, &[8], &[0.0, 99.0, 0.0, 99.0, 0.0, 99.0, 0.0, 99.0]).unwrap();
    let selected = source.view().axis_slice(0, 0, 8, 2).unwrap();
    assert_eq!(
        bits(&ops::softmax(&budget, &selected).unwrap()),
        expected_bits(&[0.25; 4])
    );
}

#[test]
fn logsumexp_retains_small_tail_and_singleton_negative_zero() {
    let budget = Budget::new(1024).unwrap();
    let small_tail = Tensor::from_slice(&budget, &[2], &[0.0, -40.0]).unwrap();
    let result = ops::logsumexp(&small_tail.view()).unwrap();
    assert!(
        result > 0.0,
        "adding the leading one first would lose this tail"
    );
    assert!((result - 4.248_354_255_291_589e-18).abs() < 1e-32);
    let singleton = Tensor::from_slice(&budget, &[1], &[-0.0]).unwrap();
    assert_eq!(
        ops::logsumexp(&singleton.view()).unwrap().to_bits(),
        (-0.0_f64).to_bits()
    );
    let same = Tensor::from_slice(&budget, &[3], &[1000.0, 1000.0, 1000.0]).unwrap();
    assert!((ops::logsumexp(&same.view()).unwrap() - 1_001.098_612_288_668_2).abs() < 1e-12);
}

#[test]
fn stable_domain_errors_precede_budget_checks_and_preserve_inputs() {
    let inputs = Budget::new(4096).unwrap();
    let full = Budget::new(0).unwrap();
    for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        let input = Tensor::from_slice(&inputs, &[3], &[0.0, 1.0, bad]).unwrap();
        let before = bits(&input);
        expect_error(ops::sigmoid(&full, &input.view()), Error::NonFiniteInput);
        expect_error(ops::softmax(&full, &input.view()), Error::NonFiniteInput);
        expect_error(ops::logsumexp(&input.view()), Error::NonFiniteInput);
        assert_eq!(bits(&input), before);
        assert_eq!(full.used_bytes(), 0);
    }
    let empty = Tensor::zeros(&inputs, &[0]).unwrap();
    expect_error(ops::softmax(&full, &empty.view()), Error::EmptyInput);
    expect_error(ops::logsumexp(&empty.view()), Error::EmptyInput);
    let scalar = Tensor::from_slice(&inputs, &[], &[0.0]).unwrap();
    expect_error(ops::softmax(&full, &scalar.view()), Error::RankMismatch);
    expect_error(ops::logsumexp(&scalar.view()), Error::RankMismatch);
    expect_error(ops::sigmoid(&full, &scalar.view()), Error::BudgetLimit);
    let vector = Tensor::from_slice(&inputs, &[1], &[0.0]).unwrap();
    expect_error(ops::softmax(&full, &vector.view()), Error::BudgetLimit);
    assert_eq!(full.used_bytes(), 0);
}
