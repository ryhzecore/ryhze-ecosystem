use super::*;
use std::cell::RefCell;
use std::process::Command;

thread_local! {
    static HOOK: RefCell<Option<(Cleanup, *mut c_void)>> = const { RefCell::new(None) };
}
pub(super) unsafe fn ax_rt_register_cleanup(cleanup: Option<Cleanup>, context: *mut c_void) -> i32 {
    HOOK.with(|hook| {
        let mut hook = hook.borrow_mut();
        if hook.is_some() || cleanup.is_none() || context.is_null() {
            return 1;
        }
        *hook = Some((cleanup.unwrap(), context));
        0
    })
}
pub(super) unsafe fn ax_rt_unregister_cleanup(context: *mut c_void) -> i32 {
    HOOK.with(|hook| {
        let mut hook = hook.borrow_mut();
        if hook
            .as_ref()
            .is_none_or(|(_, registered)| *registered != context)
        {
            return 1;
        }
        *hook = None;
        0
    })
}
pub(super) unsafe fn ax_rt_cleanup_active() -> i32 {
    HOOK.with(|hook| i32::from(hook.borrow().is_some()))
}
fn invoke_cleanup() {
    let registration = HOOK.with(|hook| hook.borrow_mut().take());
    if let Some((callback, context)) = registration {
        // The mock, like the real C trap, clears registration before invoking.
        unsafe {
            callback(context);
        }
    }
}
fn scope(
    bytes: i64,
    start: i64,
    end: i64,
    mut body: impl FnMut(*mut c_void) -> i32,
) -> (i32, [i64; 3]) {
    unsafe extern "C" fn callback<F: FnMut(*mut c_void) -> i32>(
        context: *mut c_void,
        captures: *mut c_void,
    ) -> i32 {
        unsafe { (*captures.cast::<F>())(context) }
    }
    fn invoke<F: FnMut(*mut c_void) -> i32>(
        bytes: i64,
        start: i64,
        end: i64,
        body: &mut F,
    ) -> (i32, [i64; 3]) {
        let mut error = [-1; 3];
        let status = unsafe {
            ax_tensor_v1_scope(
                bytes,
                Some(callback::<F>),
                (body as *mut F).cast(),
                start,
                end,
                error.as_mut_ptr(),
            )
        };
        (status, error)
    }
    invoke(bytes, start, end, &mut body)
}
fn values(context: &mut Context<'_>, dims: &[usize], input: &[f64]) -> u64 {
    let tensor = Tensor::from_slice(context.budget, dims, input).unwrap();
    context.own(tensor).unwrap()
}

#[test]
fn normal_and_recoverable_scope_cleanup_and_exact_first_span() {
    assert_eq!(
        scope(32, 1, 200, |context| unsafe {
            let mut owner = 0;
            assert_eq!(
                ax_tensor_v1_zeros(context, [4].as_ptr(), 1, 10, 30, &mut owner),
                0
            );
            assert_ne!(owner, 0);
            // Scope cleanup owns this unreleased allocation.
            0
        }),
        (0, [0; 3])
    );
    assert_eq!(unsafe { ax_rt_cleanup_active() }, 0);
    assert!(ACTIVE.with(|active| active.get().is_null()));
    assert_eq!(
        scope(32, 1, 200, |context| unsafe {
            let mut owner = 999;
            assert_eq!(
                ax_tensor_v1_zeros(context, [5].as_ptr(), 1, 20, 31, &mut owner),
                9
            );
            assert_eq!(owner, 0);
            assert_eq!(ax_tensor_v1_release(context, 9876, 50, 77), 6);
            0 // First actual wrapper failure remains authoritative.
        }),
        (9, [9, 20, 31])
    );
    assert_eq!(unsafe { ax_rt_cleanup_active() }, 0);
    assert_eq!(scope(32, 1, 200, |_| 999), (6, [6, 1, 200]));
    assert_eq!(scope(32, 1, 200, |_| 12), (12, [12, 1, 200]));
}

#[test]
fn invalid_budget_and_nested_priority_preserve_outer_registration() {
    assert_eq!(
        scope(-1, 2, 9, |_| panic!("invalid budget entered callback")),
        (9, [9, 2, 9])
    );
    assert_eq!(
        scope(numeric::MAX_BUDGET_BYTES as i64 + 1, 3, 10, |_| panic!(
            "oversized budget entered callback"
        )),
        (9, [9, 3, 10])
    );
    assert_eq!(
        scope(8, 1, 100, |context| {
            let registered = HOOK.with(|hook| hook.borrow().unwrap().1);
            assert_eq!(registered, context);
            assert_eq!(
                scope(-1, 22, 33, |_| panic!("nested callback entered")),
                (15, [15, 22, 33])
            );
            assert_eq!(HOOK.with(|hook| hook.borrow().unwrap().1), registered);
            let mut handle = 0;
            assert_eq!(
                unsafe { ax_tensor_v1_zeros(context, [1].as_ptr(), 1, 40, 50, &mut handle) },
                0
            );
            0
        }),
        (0, [0; 3])
    );
}

#[test]
fn invalid_context_and_null_pointer_checks_do_not_dereference() {
    let mut value = 0;
    assert_eq!(
        unsafe { ax_tensor_v1_rank(std::ptr::null_mut(), 1, 1, 2, &mut value) },
        6
    );
    assert_eq!(
        unsafe { ax_tensor_v1_rank(8_usize as *mut c_void, 1, 1, 2, &mut value) },
        6
    );
    assert_eq!(
        scope(8, 0, 99, |context| unsafe {
            assert_eq!(
                ax_tensor_v1_zeros(context, std::ptr::null(), 1, 12, 18, &mut 0),
                6
            );
            assert_eq!(
                ax_tensor_v1_zeros(context, [1].as_ptr(), 1, 30, 38, std::ptr::null_mut()),
                6
            );
            6
        }),
        (6, [6, 12, 18])
    );
    assert_eq!(
        scope(8, 0, 99, |context| unsafe {
            let mut handle = 0;
            // Rank-zero consumes one f64 and accepts an empty/null dimension list.
            assert_eq!(
                ax_tensor_v1_zeros(context, std::ptr::null(), 0, 1, 2, &mut handle),
                0
            );
            assert_eq!(ax_tensor_v1_rank(context, handle, 3, 4, &mut value), 0);
            assert_eq!(value, 0);
            0
        }),
        (0, [0; 3])
    );
}

#[test]
fn handle_reuse_is_bounded_and_never_revives_stale_values() {
    let budget = Budget::new(8).unwrap();
    let mut context = Context::new(&budget);
    let mut first = 0;
    for iteration in 0..4096 {
        let handle = values(&mut context, &[1], &[iteration as f64]);
        if iteration == 0 {
            first = handle;
        } else {
            assert!(handle > first);
            assert!(matches!(context.view(first), Err(Error::Bounds)));
        }
        assert_eq!(budget.used_bytes(), 8);
        assert_eq!(
            context.view(handle).unwrap().get_linear(0).unwrap(),
            iteration as f64
        );
        context.release(handle).unwrap();
        assert_eq!(budget.used_bytes(), 0);
        assert_eq!(context.release(handle), Err(Error::Bounds));
    }
    assert_eq!(context.release(0), Ok(()));
    let mut handles = [0; MAX_HANDLES];
    for handle in &mut handles {
        *handle = values(&mut context, &[0], &[]);
    }
    assert_eq!(context.space(), Err(Error::AllocationFailure));
    let extra = Tensor::zeros(&budget, &[1]).unwrap();
    assert_eq!(context.own(extra), Err(Error::AllocationFailure));
    assert_eq!(
        budget.used_bytes(),
        0,
        "insertion failure must drop the new owner"
    );
    context.release(handles[128]).unwrap();
    let replacement = values(&mut context, &[1], &[7.0]);
    assert_ne!(replacement, handles[128]);
    assert!(matches!(context.view(handles[128]), Err(Error::Bounds)));
    context.shutdown();
    assert_eq!(budget.used_bytes(), 0);
    assert!(context.slots.iter().all(|slot| slot.entry.is_none()));
}

#[test]
fn shared_and_exclusive_loans_guard_reads_writes_and_owner_lifetime() {
    let budget = Budget::new(32).unwrap();
    let mut context = Context::new(&budget);
    let owner = values(&mut context, &[4], &[1.0, 2.0, 3.0, 4.0]);
    let first = context
        .make_view(owner, false, Transform::Identity)
        .unwrap();
    let second = context
        .make_view(owner, false, Transform::Identity)
        .unwrap();
    assert_eq!(context.view(owner).unwrap().len(), 4);
    assert_eq!(context.set(owner, 0, 8.0), Err(Error::Bounds));
    assert_eq!(context.set(first, 0, 8.0), Err(Error::Bounds));
    assert_eq!(context.release(owner), Err(Error::Bounds));
    assert_eq!(
        context.make_view(owner, true, Transform::Identity),
        Err(Error::Bounds)
    );
    assert_eq!(
        context.make_view(first, false, Transform::Identity),
        Err(Error::Bounds)
    );
    context.release(first).unwrap();
    assert_eq!(context.set(owner, 0, 8.0), Err(Error::Bounds));
    context.release(second).unwrap();
    let exclusive = context.make_view(owner, true, Transform::Identity).unwrap();
    assert!(matches!(context.view(owner), Err(Error::Bounds)));
    assert_eq!(
        context.make_view(owner, false, Transform::Identity),
        Err(Error::Bounds)
    );
    assert_eq!(context.release(owner), Err(Error::Bounds));
    context.set(exclusive, 0, 8.0).unwrap();
    assert_eq!(context.view(exclusive).unwrap().get_linear(0).unwrap(), 8.0);
    context.release(exclusive).unwrap();
    assert_eq!(context.view(owner).unwrap().get_linear(0).unwrap(), 8.0);
    context.set(owner, 1, 9.0).unwrap();
    context.release(owner).unwrap();
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn transforms_use_real_view_mapping_and_failed_transform_leaves_no_loan() {
    let budget = Budget::new(48).unwrap();
    let mut context = Context::new(&budget);
    let owner = values(&mut context, &[2, 3], &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
    let mut axes = [0; MAX_RANK];
    axes[0] = 1;
    let transpose = context
        .make_view(owner, true, Transform::Transpose { axes, rank: 2 })
        .unwrap();
    let view = context.view(transpose).unwrap();
    assert_eq!(view.shape(), &[3, 2]);
    assert_eq!(
        (0..6)
            .map(|index| view.get_linear(index).unwrap())
            .collect::<Vec<_>>(),
        [1.0, 4.0, 2.0, 5.0, 3.0, 6.0]
    );
    context.set(transpose, 3, 50.0).unwrap();
    context.release(transpose).unwrap();
    assert_eq!(context.view(owner).unwrap().get_linear(4).unwrap(), 50.0);
    let slice = context
        .make_view(
            owner,
            true,
            Transform::Slice {
                axis: 1,
                start: 0,
                end: 3,
                step: 2,
            },
        )
        .unwrap();
    assert_eq!(context.view(slice).unwrap().shape(), &[2, 2]);
    context
        .set(slice, 3, f64::from_bits(0xfff0_0000_0000_0001))
        .unwrap();
    context.release(slice).unwrap();
    assert_eq!(
        context
            .view(owner)
            .unwrap()
            .get_linear(5)
            .unwrap()
            .to_bits(),
        numeric::CANONICAL_NAN_BITS
    );
    assert_eq!(
        context.make_view(
            owner,
            true,
            Transform::Transpose {
                axes: [0; MAX_RANK],
                rank: 2
            }
        ),
        Err(Error::InvalidPermutation)
    );
    context.set(owner, 0, 7.0).unwrap();
    assert_eq!(
        context.make_view(
            owner,
            true,
            Transform::Slice {
                axis: 0,
                start: 0,
                end: 3,
                step: 1
            }
        ),
        Err(Error::Bounds)
    );
    context.release(owner).unwrap();
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn exported_storage_views_metadata_and_split_set_checks() {
    assert_eq!(
        scope(48, 0, 300, |context| unsafe {
            let mut owner = 0;
            let data = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0];
            assert_eq!(
                ax_tensor_v1_from_array(
                    context,
                    [2, 3].as_ptr(),
                    2,
                    data.as_ptr(),
                    6,
                    1,
                    2,
                    &mut owner
                ),
                0
            );
            let mut transposed = 0;
            assert_eq!(
                ax_tensor_v1_transpose(
                    context,
                    owner,
                    1,
                    [1, 0].as_ptr(),
                    2,
                    3,
                    4,
                    &mut transposed
                ),
                0
            );
            let mut metadata = -1;
            assert_eq!(
                ax_tensor_v1_rank(context, transposed, 5, 6, &mut metadata),
                0
            );
            assert_eq!(metadata, 2);
            assert_eq!(
                ax_tensor_v1_dim(context, transposed, 0, 5, 6, &mut metadata),
                0
            );
            assert_eq!(metadata, 3);
            assert_eq!(
                ax_tensor_v1_len(context, transposed, 5, 6, &mut metadata),
                0
            );
            assert_eq!(metadata, 6);
            assert_eq!(ax_tensor_v1_check_index(context, transposed, 3, 7, 8), 0);
            assert_eq!(ax_tensor_v1_set(context, transposed, 3, 20.0, 7, 8), 0);
            assert_eq!(ax_tensor_v1_release(context, transposed, 9, 10), 0);
            let mut slice = 0;
            assert_eq!(
                ax_tensor_v1_slice(context, owner, 0, 1, 0, 3, 2, 11, 12, &mut slice),
                0
            );
            let mut value = 0.0;
            assert_eq!(ax_tensor_v1_get(context, slice, 3, 13, 14, &mut value), 0);
            assert_eq!(value, 6.0);
            assert_eq!(ax_tensor_v1_release(context, slice, 15, 16), 0);
            let mut shared = 0;
            assert_eq!(ax_tensor_v1_view(context, owner, 0, 17, 18, &mut shared), 0);
            assert_eq!(ax_tensor_v1_sum(context, shared, 19, 20, &mut value), 0);
            assert_eq!(value, 36.0);
            assert_eq!(ax_tensor_v1_release(context, shared, 21, 22), 0);
            assert_eq!(ax_tensor_v1_check_index(context, owner, 6, 40, 50), 6);
            // The caller branches on failure before its RHS; no set was performed.
            assert_eq!(ax_tensor_v1_get(context, owner, 4, 51, 52, &mut value), 0);
            assert_eq!(value, 20.0);
            6
        }),
        (6, [6, 40, 50])
    );
}

#[test]
fn exported_kernels_are_exactly_the_numerics_crate() {
    assert_eq!(
        scope(1024, 0, 300, |context| unsafe {
            let mut left = 0;
            let mut right = 0;
            let data = [1.0, 2.0, 3.0, 4.0];
            assert_eq!(
                ax_tensor_v1_from_array(
                    context,
                    [2, 2].as_ptr(),
                    2,
                    data.as_ptr(),
                    4,
                    1,
                    2,
                    &mut left
                ),
                0
            );
            assert_eq!(
                ax_tensor_v1_from_array(
                    context,
                    [2, 2].as_ptr(),
                    2,
                    data.as_ptr(),
                    4,
                    1,
                    2,
                    &mut right
                ),
                0
            );
            type Binary = unsafe extern "C" fn(*mut c_void, u64, u64, i64, i64, *mut u64) -> i32;
            for (operation, expected) in [
                (ax_tensor_v1_add as Binary, [2.0_f64, 4.0, 6.0, 8.0]),
                (ax_tensor_v1_mul as Binary, [1.0, 4.0, 9.0, 16.0]),
                (ax_tensor_v1_matmul as Binary, [7.0, 10.0, 15.0, 22.0]),
            ] {
                let mut result = 0;
                assert_eq!(operation(context, left, right, 3, 4, &mut result), 0);
                for (index, expected) in expected.into_iter().enumerate() {
                    let mut value = 0.0;
                    assert_eq!(
                        ax_tensor_v1_get(context, result, index as i64, 5, 6, &mut value),
                        0
                    );
                    assert_eq!(value.to_bits(), expected.to_bits());
                }
                assert_eq!(ax_tensor_v1_release(context, result, 7, 8), 0);
            }
            let reference_budget = Budget::new(128).unwrap();
            let reference = Tensor::from_slice(&reference_budget, &[4], &data).unwrap();
            let mut vector = 0;
            assert_eq!(
                ax_tensor_v1_from_array(
                    context,
                    [4].as_ptr(),
                    1,
                    data.as_ptr(),
                    4,
                    1,
                    2,
                    &mut vector
                ),
                0
            );
            let sigmoid = numeric::sigmoid(&reference_budget, &reference.view()).unwrap();
            let softmax = numeric::softmax(&reference_budget, &reference.view()).unwrap();
            type Unary = unsafe extern "C" fn(*mut c_void, u64, i64, i64, *mut u64) -> i32;
            for (operation, expected) in [
                (ax_tensor_v1_sigmoid as Unary, sigmoid),
                (ax_tensor_v1_softmax as Unary, softmax),
            ] {
                let mut result = 0;
                assert_eq!(operation(context, vector, 9, 10, &mut result), 0);
                for index in 0..4 {
                    let mut value = 0.0;
                    assert_eq!(
                        ax_tensor_v1_get(context, result, index, 11, 12, &mut value),
                        0
                    );
                    assert_eq!(
                        value.to_bits(),
                        expected.get_linear(index as usize).unwrap().to_bits()
                    );
                }
                assert_eq!(ax_tensor_v1_release(context, result, 13, 14), 0);
            }
            let mut value = 0.0;
            assert_eq!(
                ax_tensor_v1_logsumexp(context, vector, 15, 16, &mut value),
                0
            );
            assert_eq!(
                value.to_bits(),
                numeric::logsumexp(&reference.view()).unwrap().to_bits()
            );
            0
        }),
        (0, [0; 3])
    );
}

#[test]
fn malformed_dimensions_counts_and_permutations_have_stable_errors() {
    for (dims, count, expected) in [
        (vec![-1], 1, 2),
        (vec![65_537], 1, 2),
        (vec![65_536, 2], 2, 3),
        (vec![1; 9], 9, 1),
        (vec![], -1, 1),
    ] {
        assert_eq!(
            scope(64, 0, 100, |context| unsafe {
                ax_tensor_v1_zeros(context, dims.as_ptr(), count, 10, 20, &mut 0)
            }),
            (expected, [i64::from(expected), 10, 20])
        );
    }
    assert_eq!(
        scope(16, 0, 100, |context| unsafe {
            let mut owner = 0;
            assert_eq!(
                ax_tensor_v1_zeros(context, [2].as_ptr(), 1, 1, 2, &mut owner),
                0
            );
            assert_eq!(
                ax_tensor_v1_transpose(context, owner, 0, [-1].as_ptr(), 1, 10, 20, &mut 0),
                7
            );
            assert_eq!(
                ax_tensor_v1_slice(context, owner, 0, 0, 0, 2, 0, 30, 40, &mut 0),
                8
            );
            assert_eq!(
                ax_tensor_v1_slice(context, owner, 0, 0, -1, 2, 1, 50, 60, &mut 0),
                6
            );
            assert_eq!(
                ax_tensor_v1_slice(context, owner, 0, 9, 0, 2, 0, 60, 70, &mut 0),
                6
            );
            assert_eq!(
                ax_tensor_v1_slice(context, owner, 0, 0, 0, 9, -1, 70, 80, &mut 0),
                6
            );
            assert_eq!(
                ax_tensor_v1_slice(context, owner, 0, 0, 0, 2, -1, 80, 90, &mut 0),
                8
            );
            7
        }),
        (7, [7, 10, 20])
    );
}

#[test]
fn empty_storage_and_failed_kernels_refund_and_preserve_owners() {
    let budget = Budget::new(16).unwrap();
    let mut context = Context::new(&budget);
    let empty = values(&mut context, &[0], &[]);
    assert_eq!(
        numeric::softmax(context.budget, &context.view(empty).unwrap()).unwrap_err(),
        Error::EmptyInput
    );
    let finite = values(&mut context, &[2], &[1.0, 2.0]);
    assert_eq!(
        numeric::sigmoid(context.budget, &context.view(finite).unwrap()).unwrap_err(),
        Error::BudgetLimit
    );
    assert_eq!(budget.used_bytes(), 16);
    assert_eq!(context.view(finite).unwrap().get_linear(1).unwrap(), 2.0);
    context.shutdown();
    assert_eq!(budget.used_bytes(), 0);
    context.shutdown(); // Fatal cleanup and Drop are idempotent.
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn clear_before_fatal_cleanup_drops_metadata_and_owners_while_budget_alive() {
    let budget = Budget::new(48).unwrap();
    let mut context = Context::new(&budget);
    let owner = values(&mut context, &[6], &[1.0; 6]);
    context.make_view(owner, true, Transform::Identity).unwrap();
    let pointer = (&mut context as *mut Context<'_>).cast();
    ACTIVE.with(|active| active.set(pointer));
    assert_eq!(unsafe { ax_rt_register_cleanup(Some(cleanup), pointer) }, 0);
    invoke_cleanup();
    assert_eq!(unsafe { ax_rt_cleanup_active() }, 0);
    assert_eq!(budget.used_bytes(), 0);
    assert!(context.closed);
    assert!(context.slots.iter().all(|slot| slot.entry.is_none()));
    invoke_cleanup(); // No second invocation after registration was cleared.
    ACTIVE.with(|active| active.set(std::ptr::null_mut()));
}

#[test]
fn fatal_cleanup_child() {
    if std::env::var_os("AX_TENSOR_FATAL_CHILD").is_none() {
        return;
    }
    scope(64, 0, 100, |context| {
        let mut owner = 0;
        assert_eq!(
            unsafe { ax_tensor_v1_zeros(context, [8].as_ptr(), 1, 1, 2, &mut owner) },
            0
        );
        let mut view = 0;
        assert_eq!(
            unsafe { ax_tensor_v1_view(context, owner, 1, 3, 4, &mut view) },
            0
        );
        invoke_cleanup();
        let context = unsafe { &*context.cast::<Context<'_>>() };
        assert_eq!(context.budget.used_bytes(), 0);
        assert!(context.closed);
        assert_eq!(unsafe { ax_rt_cleanup_active() }, 0);
        println!("fatal cleanup refunded all payload before exit");
        std::process::exit(70);
    });
    unreachable!();
}
#[test]
fn fatal_exit_runs_cleanup_without_rust_stack_unwinding() {
    let output = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "tests::fatal_cleanup_child", "--nocapture"])
        .env("AX_TENSOR_FATAL_CHILD", "1")
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(70));
    assert!(String::from_utf8_lossy(&output.stdout)
        .contains("fatal cleanup refunded all payload before exit"));
}
#[test]
fn panic_child() {
    if std::env::var_os("AX_TENSOR_PANIC_CHILD").is_none() {
        return;
    }
    firewall(|| panic!("synthetic unexpected bridge panic"));
}
#[test]
fn unexpected_panic_is_terminal_not_a_tensor_error() {
    let output = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "tests::panic_child", "--nocapture"])
        .env("AX_TENSOR_PANIC_CHILD", "1")
        .output()
        .unwrap();
    assert!(!output.status.success());
    assert_ne!(output.status.code(), Some(70));
    assert!(String::from_utf8_lossy(&output.stderr).contains("synthetic unexpected bridge panic"));
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt;
        assert_eq!(output.status.signal(), Some(6));
    }
}

#[test]
fn full_arena_keeps_invalid_handles_and_views_checked_before_capacity() {
    assert_eq!(
        scope(0, 0, 100, |context| unsafe {
            let mut first = 0;
            for index in 0..MAX_HANDLES {
                let mut handle = 0;
                assert_eq!(
                    ax_tensor_v1_zeros(context, [0].as_ptr(), 1, 1, 2, &mut handle),
                    0
                );
                if index == 0 {
                    first = handle;
                }
            }
            assert_eq!(ax_tensor_v1_add(context, 0, first, 10, 20, &mut 0), 6);
            assert_eq!(ax_tensor_v1_sigmoid(context, 0, 20, 30, &mut 0), 6);
            assert_eq!(ax_tensor_v1_view(context, 0, 0, 30, 40, &mut 0), 6);
            assert_eq!(
                ax_tensor_v1_zeros(context, [65_537].as_ptr(), 1, 40, 50, &mut 0),
                2
            );
            assert_eq!(
                ax_tensor_v1_transpose(context, first, 0, [-1].as_ptr(), 1, 50, 60, &mut 0),
                7
            );
            assert_eq!(ax_tensor_v1_view(context, first, 0, 60, 70, &mut 0), 10);
            assert_eq!(ax_tensor_v1_add(context, first, first, 70, 80, &mut 0), 10);
            // Valid handles followed by full capacity wins over an empty-vector
            // domain error. This path is defensive ABI behavior; checked source
            // cannot hold all 256 entries plus another result local.
            assert_eq!(ax_tensor_v1_softmax(context, first, 80, 90, &mut 0), 10);
            6
        }),
        (6, [6, 10, 20])
    );
}

#[test]
fn all_library_error_tags_match_the_stable_source_contract() {
    for (error, expected) in [
        (Error::RankLimit, 1),
        (Error::ExtentLimit, 2),
        (Error::ElementLimit, 3),
        (Error::ShapeMismatch, 4),
        (Error::RankMismatch, 5),
        (Error::Bounds, 6),
        (Error::InvalidPermutation, 7),
        (Error::InvalidStep, 8),
        (Error::BudgetLimit, 9),
        (Error::AllocationFailure, 10),
        (Error::WorkLimit, 11),
        (Error::NonFiniteInput, 12),
        (Error::EmptyInput, 13),
        (Error::NumericalFailure, 14),
    ] {
        assert_eq!(error_code(error), expected);
    }
}

#[test]
fn rejected_recursive_scopes_do_not_accumulate_arena_frames() {
    fn recurse(depth: usize) {
        assert_eq!(scope(-1, 22, 33, |_| unreachable!()), (15, [15, 22, 33]));
        if depth != 0 {
            recurse(depth - 1);
        }
    }
    std::thread::Builder::new()
        .stack_size(1024 * 1024)
        .spawn(|| {
            assert_eq!(
                scope(8, 0, 100, |context| {
                    let mut handle = 0;
                    assert_eq!(
                        unsafe { ax_tensor_v1_zeros(context, [1].as_ptr(), 1, 1, 2, &mut handle) },
                        0
                    );
                    recurse(64);
                    assert_eq!(unsafe { ax_tensor_v1_release(context, handle, 3, 4) }, 0);
                    0
                }),
                (0, [0; 3])
            );
        })
        .unwrap()
        .join()
        .unwrap();
}
