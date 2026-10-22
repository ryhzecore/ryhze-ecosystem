use axeon_numerics::{
    Budget, DType, Device, Error, Layout, Shape, Tensor, CANONICAL_NAN_BITS, MAX_BUDGET_BYTES,
    MAX_ELEMENTS, MAX_EXTENT, MAX_RANK,
};

#[test]
fn scalar_and_empty_shapes_have_distinct_element_counts() {
    let scalar = Shape::new(&[]).unwrap();
    assert_eq!(scalar.rank(), 0);
    assert_eq!(scalar.len(), 1);
    assert!(!scalar.is_empty());
    let empty = Shape::new(&[MAX_EXTENT, 0, MAX_EXTENT]).unwrap();
    assert_eq!(empty.len(), 0);
    assert_eq!(empty.strides(), [0, 0, 0]);
    assert!(empty.is_empty());
    assert_eq!(Shape::new(&[2, 3, 4]).unwrap().strides(), [12, 4, 1]);
}

#[test]
fn all_extents_are_validated_before_empty_shape_shortcut() {
    assert_eq!(Shape::new(&[0, MAX_EXTENT + 1]), Err(Error::ExtentLimit));
    assert_eq!(Shape::new(&[0, usize::MAX]), Err(Error::ExtentLimit));
    assert_eq!(Shape::new(&[0; MAX_RANK + 1]), Err(Error::RankLimit));
    assert_eq!(Shape::new(&[257, 256]), Err(Error::ElementLimit));
    assert_eq!(
        Shape::new(&[MAX_EXTENT; MAX_RANK]),
        Err(Error::ElementLimit)
    );
    assert_eq!(Shape::new(&[MAX_EXTENT]).unwrap().len(), MAX_ELEMENTS);
}

#[test]
fn live_budget_is_shared_and_refunded_after_moves_and_drops() {
    let budget = Budget::new(32).unwrap();
    let first = Tensor::zeros(&budget, &[2]).unwrap();
    let first_identity = first.allocation_id();
    let second = Tensor::zeros(&budget, &[2]).unwrap();
    assert_ne!(first_identity, second.allocation_id());
    assert_eq!(budget.used_bytes(), 32);
    assert_eq!(
        Tensor::zeros(&budget, &[1]).unwrap_err(),
        Error::BudgetLimit
    );
    assert_eq!(budget.used_bytes(), 32);
    let moved = first;
    assert_eq!(moved.allocation_id(), first_identity);
    drop(second);
    assert_eq!(budget.used_bytes(), 16);
    let replacement = Tensor::zeros(&budget, &[2]).unwrap();
    assert_ne!(replacement.allocation_id(), first_identity);
    drop(moved);
    drop(replacement);
    assert_eq!(budget.used_bytes(), 0);
    assert_eq!(budget.limit_bytes(), 32);
}

#[test]
fn budget_and_shape_failures_leave_accounting_unchanged() {
    assert_eq!(
        Budget::new(MAX_BUDGET_BYTES + 1).unwrap_err(),
        Error::BudgetLimit
    );
    let budget = Budget::new(8).unwrap();
    assert_eq!(
        Tensor::from_slice(&budget, &[2], &[1.0]).unwrap_err(),
        Error::ShapeMismatch
    );
    assert_eq!(
        Tensor::zeros(&budget, &[MAX_EXTENT + 1]).unwrap_err(),
        Error::ExtentLimit
    );
    assert_eq!(
        Tensor::zeros(&budget, &[2]).unwrap_err(),
        Error::BudgetLimit
    );
    assert_eq!(budget.used_bytes(), 0);
    let scalar = Tensor::from_slice(&budget, &[], &[-0.0]).unwrap();
    assert_eq!(scalar.get(&[]).unwrap().to_bits(), (-0.0_f64).to_bits());
    assert_eq!(scalar.get(&[0]), Err(Error::RankMismatch));
    drop(scalar);
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn maximum_owned_shape_allocates_exact_requested_payload() {
    let budget = Budget::new(MAX_ELEMENTS * 8).unwrap();
    let tensor = Tensor::zeros(&budget, &[256, 256]).unwrap();
    assert_eq!(tensor.len(), MAX_ELEMENTS);
    assert_eq!(budget.used_bytes(), MAX_ELEMENTS * 8);
    assert_eq!(tensor.get(&[255, 255]), Ok(0.0));
    assert_eq!(tensor.get(&[256, 0]), Err(Error::Bounds));
    drop(tensor);
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn zero_payload_owners_need_no_budget_and_still_have_unique_identity() {
    let budget = Budget::new(0).unwrap();
    let first = Tensor::zeros(&budget, &[0, MAX_EXTENT]).unwrap();
    let second = Tensor::zeros(&budget, &[0]).unwrap();
    assert_eq!(budget.used_bytes(), 0);
    assert_ne!(first.allocation_id(), second.allocation_id());
    assert_eq!(first.view().offset(), 0);
    assert_eq!(first.view().strides(), [0, 0]);
    assert_eq!(first.view().as_slice(), Some([].as_slice()));
    assert_eq!(first.get_linear(0), Err(Error::Bounds));
    assert_eq!(first.get(&[0, 0]), Err(Error::Bounds));
    assert_eq!(Tensor::zeros(&budget, &[]).unwrap_err(), Error::BudgetLimit);
}

#[test]
fn canonical_storage_preserves_every_non_nan_representation() {
    let budget = Budget::new(128).unwrap();
    let patterns = [
        0,
        1_u64 << 63,
        1,
        0x000f_ffff_ffff_ffff,
        0x0010_0000_0000_0000,
        0x7fef_ffff_ffff_ffff,
        0x7ff0_0000_0000_0000,
        0xfff0_0000_0000_0000,
        0x7ff0_0000_0000_0001,
        0xfff8_1234_5678_9abc,
    ];
    let input: Vec<f64> = patterns.into_iter().map(f64::from_bits).collect();
    let mut tensor = Tensor::from_slice(&budget, &[input.len()], &input).unwrap();
    let output: Vec<u64> = tensor.as_slice().iter().map(|v| v.to_bits()).collect();
    assert_eq!(&output[..8], &patterns[..8]);
    assert_eq!(&output[8..], &[CANONICAL_NAN_BITS; 2]);
    tensor
        .set_linear(0, f64::from_bits(0xfff0_0000_0000_0001))
        .unwrap();
    tensor
        .set(&[1], f64::from_bits(0x7fff_ffff_ffff_ffff))
        .unwrap();
    assert_eq!(tensor.get_linear(0).unwrap().to_bits(), CANONICAL_NAN_BITS);
    assert_eq!(tensor.get_linear(1).unwrap().to_bits(), CANONICAL_NAN_BITS);
}

#[test]
fn transpose_changes_logical_order_without_allocation_or_identity_change() {
    let budget = Budget::new(48).unwrap();
    let tensor = Tensor::from_slice(&budget, &[2, 3], &[0., 1., 2., 3., 4., 5.]).unwrap();
    let transposed = tensor.view().transpose(&[1, 0]).unwrap();
    assert_eq!(transposed.shape(), [3, 2]);
    assert_eq!(transposed.strides(), [1, 3]);
    assert_eq!(transposed.layout(), Layout::Strided);
    assert_eq!(transposed.allocation_id(), tensor.allocation_id());
    assert_eq!(transposed.as_slice(), None);
    let values: Vec<_> = (0..6).map(|i| transposed.get_linear(i).unwrap()).collect();
    assert_eq!(values, [0., 3., 1., 4., 2., 5.]);
    assert_eq!(budget.used_bytes(), 48);
    let restored = transposed.transpose(&[1, 0]).unwrap();
    assert_eq!(restored.layout(), Layout::Dense);
    assert_eq!(restored.as_slice(), Some(tensor.as_slice()));
}

#[test]
fn positive_step_slices_compose_with_transposes() {
    let budget = Budget::new(96).unwrap();
    let input: Vec<_> = (0..12).map(f64::from).collect();
    let tensor = Tensor::from_slice(&budget, &[3, 4], &input).unwrap();
    let view = tensor
        .view()
        .axis_slice(1, 1, 4, 2)
        .unwrap()
        .transpose(&[1, 0])
        .unwrap();
    assert_eq!(view.shape(), [2, 3]);
    assert_eq!(view.strides(), [2, 4]);
    assert_eq!(view.offset(), 1);
    let values: Vec<_> = (0..view.len())
        .map(|i| view.get_linear(i).unwrap())
        .collect();
    assert_eq!(values, [1., 5., 9., 3., 7., 11.]);
    assert_eq!(view.get(&[1, 2]), Ok(11.));
    assert_eq!(view.get_linear(6), Err(Error::Bounds));
}

#[test]
fn empty_views_canonicalize_descriptors_even_after_nonzero_offsets() {
    let budget = Budget::new(48).unwrap();
    let tensor = Tensor::zeros(&budget, &[2, 3]).unwrap();
    let empty = tensor
        .view()
        .axis_slice(1, 2, 3, 1)
        .unwrap()
        .axis_slice(0, 2, 2, usize::MAX)
        .unwrap();
    assert_eq!(empty.shape(), [0, 1]);
    assert_eq!(empty.offset(), 0);
    assert_eq!(empty.strides(), [0, 0]);
    assert_eq!(empty.layout(), Layout::Dense);
    let transposed = empty.transpose(&[1, 0]).unwrap();
    assert_eq!(transposed.strides(), [0, 0]);
    assert_eq!(transposed.offset(), 0);
    assert_eq!(transposed.get_linear(0), Err(Error::Bounds));
}

#[test]
fn singleton_axes_and_huge_positive_steps_report_dense_layout_accurately() {
    let budget = Budget::new(24).unwrap();
    let tensor = Tensor::from_slice(&budget, &[1, 3], &[1., 2., 3.]).unwrap();
    let transposed = tensor.view().transpose(&[1, 0]).unwrap();
    assert_eq!(transposed.layout(), Layout::Dense);
    assert_eq!(transposed.as_slice(), Some([1., 2., 3.].as_slice()));
    let single = transposed.axis_slice(0, 1, 3, usize::MAX).unwrap();
    assert_eq!(single.shape(), [1, 1]);
    assert_eq!(single.get_linear(0), Ok(2.));
    assert_eq!(single.layout(), Layout::Dense);
    assert_eq!(single.as_slice(), Some([2.].as_slice()));
}

#[test]
fn invalid_view_transformations_are_explicit_failures() {
    let budget = Budget::new(48).unwrap();
    let tensor = Tensor::zeros(&budget, &[2, 3]).unwrap();
    let view = tensor.view();
    assert_eq!(
        view.transpose(&[0, 0]).unwrap_err(),
        Error::InvalidPermutation
    );
    assert_eq!(view.transpose(&[0]).unwrap_err(), Error::InvalidPermutation);
    assert_eq!(
        view.transpose(&[0, 2]).unwrap_err(),
        Error::InvalidPermutation
    );
    assert_eq!(view.axis_slice(0, 0, 2, 0).unwrap_err(), Error::InvalidStep);
    assert_eq!(view.axis_slice(2, 0, 0, 1).unwrap_err(), Error::Bounds);
    assert_eq!(view.axis_slice(0, 2, 1, 1).unwrap_err(), Error::Bounds);
    assert_eq!(view.axis_slice(0, 0, 3, 1).unwrap_err(), Error::Bounds);
    assert_eq!(view.get(&[0]), Err(Error::RankMismatch));
    assert_eq!(view.get(&[0, usize::MAX]), Err(Error::Bounds));
    assert_eq!(view.get_linear(usize::MAX), Err(Error::Bounds));
    assert_eq!(budget.used_bytes(), 48);
}

#[test]
fn mutable_transformed_views_update_only_selected_storage_and_canonicalize() {
    let budget = Budget::new(96).unwrap();
    let mut tensor = Tensor::zeros(&budget, &[3, 4]).unwrap();
    let identity = tensor.allocation_id();
    {
        let mut view = tensor
            .view_mut()
            .axis_slice(1, 1, 4, 2)
            .unwrap()
            .transpose(&[1, 0])
            .unwrap();
        assert_eq!(view.allocation_id(), identity);
        assert_eq!(view.layout(), Layout::Strided);
        assert_eq!(view.dtype(), DType::F64);
        assert_eq!(view.device(), Device::Cpu);
        view.set(&[0, 1], 5.).unwrap();
        view.set_linear(5, f64::from_bits(0xfff0_0000_0000_0001))
            .unwrap();
        assert_eq!(view.view().get(&[0, 1]), Ok(5.));
        assert_eq!(view.get_linear(5).unwrap().to_bits(), CANONICAL_NAN_BITS);
        assert_eq!(view.set(&[2, 0], 99.), Err(Error::Bounds));
        assert_eq!(view.set_linear(6, 99.), Err(Error::Bounds));
    }
    assert_eq!(tensor.get(&[1, 1]), Ok(5.));
    assert_eq!(tensor.get(&[2, 3]).unwrap().to_bits(), CANONICAL_NAN_BITS);
    for index in 0..12 {
        if index != 5 && index != 11 {
            assert_eq!(tensor.get_linear(index), Ok(0.));
        }
    }
    assert_eq!(budget.used_bytes(), 96);
}

#[test]
fn scalar_views_accept_only_scalar_coordinates_and_permutations() {
    let budget = Budget::new(8).unwrap();
    let mut tensor = Tensor::from_slice(&budget, &[], &[3.5]).unwrap();
    assert_eq!(tensor.dtype(), DType::F64);
    assert_eq!(tensor.device(), Device::Cpu);
    assert_eq!(tensor.layout(), Layout::Dense);
    let view = tensor.view().transpose(&[]).unwrap();
    assert_eq!(view.get(&[]), Ok(3.5));
    assert_eq!(view.get_linear(1), Err(Error::Bounds));
    assert_eq!(view.axis_slice(0, 0, 0, 1).unwrap_err(), Error::Bounds);
    tensor
        .view_mut()
        .transpose(&[])
        .unwrap()
        .set(&[], -0.)
        .unwrap();
    assert_eq!(
        tensor.get_linear(0).unwrap().to_bits(),
        (-0.0_f64).to_bits()
    );
}

#[test]
fn exhaustive_small_views_preserve_coordinates_and_mutable_injectivity() {
    // Compare public reads/writes against direct original matrix coordinates,
    // independently of the implementation's offset/stride calculation.
    let budget = Budget::new(256).unwrap();
    let mut cases = 0;
    for rows in 0..=4 {
        for columns in 0..=4 {
            let values: Vec<_> = (0..rows * columns).map(|i| i as f64).collect();
            let owner = Tensor::from_slice(&budget, &[rows, columns], &values).unwrap();
            for transposed in [false, true] {
                let axes = if transposed { [1, 0] } else { [0, 1] };
                let base = owner.view().transpose(&axes).unwrap();
                for axis in 0..2 {
                    for start in 0..=base.shape()[axis] {
                        for end in start..=base.shape()[axis] {
                            for step in [1, 2, 3, 4, usize::MAX] {
                                let selected: Vec<_> = (start..end).step_by(step).collect();
                                let mut dims = [base.shape()[0], base.shape()[1]];
                                dims[axis] = selected.len();
                                let view = base.axis_slice(axis, start, end, step).unwrap();
                                assert_eq!(view.shape(), dims);
                                let mut expected = Vec::new();
                                for row in 0..dims[0] {
                                    for column in 0..dims[1] {
                                        let source_row =
                                            if axis == 0 { selected[row] } else { row };
                                        let source_column =
                                            if axis == 1 { selected[column] } else { column };
                                        let original_index = if transposed {
                                            source_column * columns + source_row
                                        } else {
                                            source_row * columns + source_column
                                        };
                                        assert_eq!(
                                            view.get(&[row, column]),
                                            Ok(original_index as f64)
                                        );
                                        expected.push(original_index);
                                    }
                                }
                                assert_eq!(view.len(), expected.len());
                                for (linear, &original_index) in expected.iter().enumerate() {
                                    assert_eq!(view.get_linear(linear), Ok(original_index as f64));
                                }
                                assert_eq!(view.get_linear(expected.len()), Err(Error::Bounds));
                                let mut changed = Tensor::zeros(&budget, &[rows, columns]).unwrap();
                                {
                                    let mut mutable = changed
                                        .view_mut()
                                        .transpose(&axes)
                                        .unwrap()
                                        .axis_slice(axis, start, end, step)
                                        .unwrap();
                                    assert_eq!(mutable.shape(), dims);
                                    for (linear, &original_index) in expected.iter().enumerate() {
                                        mutable
                                            .set_linear(linear, (original_index + 1) as f64)
                                            .unwrap();
                                    }
                                }
                                for original_index in 0..rows * columns {
                                    let occurrences =
                                        expected.iter().filter(|&&i| i == original_index).count();
                                    assert!(occurrences <= 1, "mutable view must be injective");
                                    let value = if occurrences == 1 {
                                        (original_index + 1) as f64
                                    } else {
                                        0.0
                                    };
                                    assert_eq!(changed.get_linear(original_index), Ok(value));
                                }
                                cases += 1;
                            }
                        }
                    }
                }
            }
        }
    }
    assert_eq!(cases, 3500);
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn rank_one_views_support_chained_slices_and_exclusive_updates() {
    let budget = Budget::new(56).unwrap();
    let mut owner = Tensor::from_slice(&budget, &[7], &[0., 1., 2., 3., 4., 5., 6.]).unwrap();
    let shared = owner
        .view()
        .transpose(&[0])
        .unwrap()
        .axis_slice(0, 1, 7, 2)
        .unwrap()
        .axis_slice(0, 1, 3, 1)
        .unwrap();
    assert_eq!(shared.shape(), [2]);
    assert_eq!(shared.get(&[0]), Ok(3.));
    assert_eq!(shared.get_linear(1), Ok(5.));
    assert_eq!(shared.layout(), Layout::Strided);
    assert_eq!(shared.get_linear(2), Err(Error::Bounds));
    {
        let mut mutable = owner
            .view_mut()
            .transpose(&[0])
            .unwrap()
            .axis_slice(0, 1, 7, 2)
            .unwrap()
            .axis_slice(0, 1, 3, 1)
            .unwrap();
        mutable.set(&[0], 30.).unwrap();
        mutable.set_linear(1, 50.).unwrap();
    }
    assert_eq!(owner.as_slice(), [0., 1., 2., 30., 4., 50., 6.]);
    let empty = owner.view().axis_slice(0, 7, 7, 1).unwrap();
    assert_eq!(empty.strides(), [0]);
    assert_eq!(empty.offset(), 0);
    assert_eq!(empty.get(&[0]), Err(Error::Bounds));
}

#[derive(Clone, Copy)]
struct AxisSlice {
    axis: usize,
    start: usize,
    end: usize,
    step: usize,
}

fn check_rank_three_case(
    budget: &Budget,
    dimensions: [usize; 3],
    axes: [usize; 3],
    slices: &[AxisSlice],
) {
    // The oracle uses explicit source-coordinate tuples and selected axis
    // labels. It never reads or recomputes the view's offset or strides.
    let mut source = Vec::new();
    for i in 0..dimensions[0] {
        for j in 0..dimensions[1] {
            for k in 0..dimensions[2] {
                source.push(([i, j, k], (100 * i + 10 * j + k) as f64));
            }
        }
    }
    let values: Vec<_> = source.iter().map(|&(_, value)| value).collect();
    let owner = Tensor::from_slice(budget, &dimensions, &values).unwrap();
    let mut view = owner.view().transpose(&axes).unwrap();
    let mut selected: [Vec<usize>; 3] =
        std::array::from_fn(|axis| (0..dimensions[axes[axis]]).collect());
    for &slice in slices {
        selected[slice.axis] = selected[slice.axis][slice.start..slice.end]
            .iter()
            .step_by(slice.step)
            .copied()
            .collect();
        view = view
            .axis_slice(slice.axis, slice.start, slice.end, slice.step)
            .unwrap();
    }
    let logical_shape = selected.each_ref().map(|axis| axis.len());
    assert_eq!(view.shape(), logical_shape);
    let mut expected_positions = Vec::new();
    for (i, &a) in selected[0].iter().enumerate() {
        for (j, &b) in selected[1].iter().enumerate() {
            for (k, &c) in selected[2].iter().enumerate() {
                let mut original = [0; 3];
                original[axes[0]] = a;
                original[axes[1]] = b;
                original[axes[2]] = c;
                let position = source
                    .iter()
                    .position(|&(coordinate, _)| coordinate == original)
                    .unwrap();
                assert_eq!(view.get(&[i, j, k]), Ok(source[position].1));
                expected_positions.push(position);
            }
        }
    }
    assert_eq!(view.len(), expected_positions.len());
    for (linear, &position) in expected_positions.iter().enumerate() {
        assert_eq!(view.get_linear(linear), Ok(source[position].1));
    }
    assert_eq!(
        view.get_linear(expected_positions.len()),
        Err(Error::Bounds)
    );
    let contiguous = expected_positions
        .windows(2)
        .all(|pair| pair[1] == pair[0] + 1);
    assert_eq!(
        view.layout(),
        if contiguous {
            Layout::Dense
        } else {
            Layout::Strided
        }
    );
    if contiguous {
        let expected_values: Vec<_> = expected_positions.iter().map(|&p| source[p].1).collect();
        assert_eq!(view.as_slice(), Some(expected_values.as_slice()));
    } else {
        assert_eq!(view.as_slice(), None);
    }
    let mut changed = Tensor::zeros(budget, &dimensions).unwrap();
    {
        let mut mutable = changed.view_mut().transpose(&axes).unwrap();
        for &slice in slices {
            mutable = mutable
                .axis_slice(slice.axis, slice.start, slice.end, slice.step)
                .unwrap();
        }
        assert_eq!(mutable.shape(), logical_shape);
        for linear in 0..expected_positions.len() {
            mutable.set_linear(linear, (linear + 1) as f64).unwrap();
        }
    }
    for physical in 0..source.len() {
        let mut occurrences = expected_positions
            .iter()
            .enumerate()
            .filter(|&(_, &position)| position == physical);
        let expected = occurrences
            .next()
            .map_or(0., |(linear, _)| (linear + 1) as f64);
        assert!(
            occurrences.next().is_none(),
            "mutable coordinates must be injective"
        );
        assert_eq!(changed.get_linear(physical), Ok(expected));
    }
}

#[test]
fn rank_three_permutations_and_chained_slices_preserve_independent_coordinates() {
    let budget = Budget::new(432).unwrap();
    let permutations = [
        [0, 1, 2],
        [0, 2, 1],
        [1, 0, 2],
        [1, 2, 0],
        [2, 0, 1],
        [2, 1, 0],
    ];
    let mut single_axis_cases = 0;
    let mut chained_cases = 0;
    for a in 0..=3 {
        for b in 0..=3 {
            for c in 0..=3 {
                let dimensions = [a, b, c];
                for axes in permutations {
                    for axis in 0..3 {
                        let extent = dimensions[axes[axis]];
                        for (start, end, step) in [
                            (0, extent, 1),
                            (0, extent, 2),
                            (extent.min(1), extent, 2),
                            (extent, extent, usize::MAX),
                        ] {
                            check_rank_three_case(
                                &budget,
                                dimensions,
                                axes,
                                &[AxisSlice {
                                    axis,
                                    start,
                                    end,
                                    step,
                                }],
                            );
                            single_axis_cases += 1;
                        }
                        let next_axis = (axis + 1) % 3;
                        let next_extent = dimensions[axes[next_axis]];
                        check_rank_three_case(
                            &budget,
                            dimensions,
                            axes,
                            &[
                                AxisSlice {
                                    axis,
                                    start: 0,
                                    end: extent,
                                    step: 2,
                                },
                                AxisSlice {
                                    axis: next_axis,
                                    start: next_extent.min(1),
                                    end: next_extent,
                                    step: 2,
                                },
                            ],
                        );
                        chained_cases += 1;
                    }
                }
            }
        }
    }
    assert_eq!(single_axis_cases, 4608);
    assert_eq!(chained_cases, 1152);
    assert_eq!(budget.used_bytes(), 0);
}
