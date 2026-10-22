//! Type-independent lexical and affine state shared by semantic checking.
//!
//! The type checker decides which operations require an owner or a loan. This
//! layer records their consequences without assuming a runtime representation,
//! copy capability, numeric operation, layout or size for a local's type.
//! Concrete checking stores existing IR locals without reallocating them at the
//! output boundary; symbolic checking can use a different local descriptor.
use std::collections::HashMap;

use crate::diagnostic::Span;

pub(super) type LocalId = usize;
pub(super) type Flow = u8;
pub(super) const NEXT: Flow = 1;
pub(super) const RETURN: Flow = 2;
pub(super) const BREAK: Flow = 4;
pub(super) const CONTINUE: Flow = 8;

pub(super) trait LocalBinding {
    fn name(&self) -> &str;
}

impl LocalBinding for crate::ir::Local {
    fn name(&self) -> &str {
        &self.name
    }
}

#[derive(Default)]
struct Scope {
    bindings: HashMap<String, LocalId>,
    // Erroneous duplicate declarations still belong to this scope: any loan
    // attached to their fresh IDs must be removed during diagnostic recovery.
    locals: Vec<LocalId>,
}

#[derive(Clone)]
pub(super) struct Loan {
    pub owner: LocalId,
    pub slice: LocalId,
    pub mutable: bool,
    pub span: Span,
}

#[derive(Clone, Default)]
pub(super) struct Ownership {
    pub moved: HashMap<LocalId, Span>,
    pub partial: HashMap<LocalId, Vec<(Vec<usize>, Span)>>,
    pub loans: Vec<Loan>,
}

impl Ownership {
    pub fn move_site(&self, owner: LocalId, path: &[usize]) -> Option<Span> {
        self.moved.get(&owner).copied().or_else(|| {
            self.partial.get(&owner).and_then(|paths| {
                paths
                    .iter()
                    .find(|(moved, _)| moved.starts_with(path) || path.starts_with(moved))
                    .map(|(_, span)| *span)
            })
        })
    }

    pub fn conflicting_loan(&self, owner: LocalId, exclusive: bool) -> Option<&Loan> {
        self.loans
            .iter()
            .find(|loan| loan.owner == owner && (exclusive || loan.mutable))
    }

    pub fn join(mut left: Self, right: Self) -> Self {
        // Available on every incoming path means moved on no incoming path.
        // Preserve the first move/loan's provenance, including diagnostic order.
        for (owner, span) in right.moved {
            left.moved.entry(owner).or_insert(span);
        }
        for (owner, paths) in right.partial {
            let left_paths = left.partial.entry(owner).or_default();
            for (path, span) in paths {
                if !left_paths.iter().any(|(existing, _)| *existing == path) {
                    left_paths.push((path, span));
                }
            }
        }
        for loan in right.loans {
            if !left
                .loans
                .iter()
                .any(|existing| existing.slice == loan.slice)
            {
                left.loans.push(loan);
            }
        }
        left
    }

    pub fn join_continuing(
        entry: Self,
        left: Self,
        left_flow: Flow,
        right: Self,
        right_flow: Flow,
    ) -> Self {
        match (left_flow & NEXT != 0, right_flow & NEXT != 0) {
            (true, true) => Self::join(left, right),
            (true, false) => left,
            (false, true) => right,
            (false, false) => entry,
        }
    }
}

pub(super) struct State<L> {
    pub locals: Vec<L>,
    pub ownership: Ownership,
    pub loop_depth: usize,
    // First protected-body local ID and enclosing loop depth. Captured owners
    // must remain available to recovery paths; local owners can be transferred.
    pub tensor_body: Option<(LocalId, usize)>,
    scopes: Vec<Scope>,
    local_loop_depth: Vec<usize>,
}

impl<L: LocalBinding> State<L> {
    pub fn new() -> Self {
        Self {
            locals: Vec::new(),
            ownership: Ownership::default(),
            loop_depth: 0,
            tensor_body: None,
            scopes: vec![Scope::default()],
            local_loop_depth: Vec::new(),
        }
    }

    /// Keep both the old binding and the fresh erroneous local when duplicated.
    /// Diagnostic wording and type rules belong to the semantic checker.
    pub fn declare(&mut self, local: L) -> (LocalId, Option<LocalId>) {
        let scope = self.scopes.last_mut().expect("function always has a scope");
        let id = self.locals.len();
        let previous = scope.bindings.get(local.name()).copied();
        if previous.is_none() {
            scope.bindings.insert(local.name().to_owned(), id);
        }
        scope.locals.push(id);
        self.local_loop_depth.push(self.loop_depth);
        self.locals.push(local);
        (id, previous)
    }

    pub fn resolve(&self, name: &str) -> Option<LocalId> {
        self.scopes
            .iter()
            .rev()
            .find_map(|scope| scope.bindings.get(name).copied())
    }

    pub fn scope_depth(&self) -> usize {
        self.scopes.len()
    }

    pub fn enter_scope(&mut self) {
        self.scopes.push(Scope::default());
    }

    pub fn leave_scope(&mut self) {
        let scope = self.scopes.pop().expect("nested scope exists");
        self.ownership
            .loans
            .retain(|loan| !scope.locals.contains(&loan.slice));
        for local in scope.locals {
            self.ownership.moved.remove(&local);
            self.ownership.partial.remove(&local);
        }
    }

    pub fn loop_move_allowed(&self, owner: LocalId) -> bool {
        self.local_loop_depth[owner] == self.loop_depth
    }

    pub fn protected_move_allowed(&self, owner: LocalId) -> bool {
        self.tensor_body.is_none_or(|(first, _)| owner >= first)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // These identities have no AST Type, physical layout, scalar operations or
    // copy semantics. The same state machine accepts them without inventing any.
    struct OpaqueLocal {
        name: String,
        parameter: usize,
    }
    impl LocalBinding for OpaqueLocal {
        fn name(&self) -> &str {
            &self.name
        }
    }
    fn declare(state: &mut State<OpaqueLocal>, name: &str, parameter: usize) -> LocalId {
        state
            .declare(OpaqueLocal {
                name: name.into(),
                parameter,
            })
            .0
    }
    fn site(index: usize) -> Span {
        Span::new(index, index + 1)
    }

    #[test]
    fn opaque_locals_preserve_identity_and_lexical_shadowing() {
        let mut state = State::new();
        let outer = declare(&mut state, "value", 0);
        state.enter_scope();
        let inner = declare(&mut state, "value", 1);
        assert_eq!(state.resolve("value"), Some(inner));
        assert_eq!(state.locals[outer].parameter, 0);
        assert_eq!(state.locals[inner].parameter, 1);
        state.leave_scope();
        assert_eq!(state.resolve("value"), Some(outer));
        assert_eq!(state.scope_depth(), 1);
    }

    #[test]
    fn scope_exit_removes_duplicate_local_loans_without_releasing_outer_loans() {
        let mut state = State::new();
        let owner = declare(&mut state, "owner", 0);
        let outer = declare(&mut state, "loan", 0);
        state.ownership.loans.push(Loan {
            owner,
            slice: outer,
            mutable: false,
            span: site(1),
        });
        state.enter_scope();
        let inner = declare(&mut state, "loan", 0);
        let (duplicate, previous) = state.declare(OpaqueLocal {
            name: "loan".into(),
            parameter: 1,
        });
        assert_eq!(previous, Some(inner));
        assert_eq!(state.resolve("loan"), Some(inner));
        state.ownership.loans.push(Loan {
            owner,
            slice: duplicate,
            mutable: true,
            span: site(2),
        });
        state.ownership.moved.insert(duplicate, site(3));
        state
            .ownership
            .partial
            .insert(inner, vec![(vec![0], site(4))]);
        state.leave_scope();
        assert_eq!(state.resolve("loan"), Some(outer));
        assert_eq!(state.ownership.loans.len(), 1);
        assert_eq!(state.ownership.loans[0].slice, outer);
        assert!(!state.ownership.moved.contains_key(&duplicate));
        assert!(!state.ownership.partial.contains_key(&inner));
    }

    #[test]
    fn partial_moves_exclude_ancestors_and_descendants_but_preserve_siblings() {
        let mut state = Ownership::default();
        state.partial.insert(0, vec![(vec![1, 2], site(1))]);
        for path in [&[][..], &[1], &[1, 2], &[1, 2, 3]] {
            assert_eq!(state.move_site(0, path), Some(site(1)));
        }
        assert_eq!(state.move_site(0, &[1, 3]), None);
        assert_eq!(state.move_site(0, &[0]), None);
        state.moved.insert(0, site(2));
        assert_eq!(state.move_site(0, &[0]), Some(site(2)));
        assert_eq!(state.move_site(0, &[1, 2]), Some(site(2)));
    }

    #[test]
    fn only_continuing_exits_contribute_ownership_at_branch_joins() {
        let entry = Ownership::default();
        let mut left = entry.clone();
        left.moved.insert(0, site(1));
        let mut right = entry.clone();
        right.moved.insert(1, site(2));
        let result =
            Ownership::join_continuing(entry.clone(), left.clone(), RETURN, right.clone(), NEXT);
        assert_eq!(result.move_site(0, &[]), None);
        assert_eq!(result.move_site(1, &[]), Some(site(2)));
        let result = Ownership::join_continuing(
            entry.clone(),
            left.clone(),
            NEXT,
            right.clone(),
            RETURN | BREAK,
        );
        assert_eq!(result.move_site(0, &[]), Some(site(1)));
        assert_eq!(result.move_site(1, &[]), None);
        let result = Ownership::join_continuing(
            entry.clone(),
            left.clone(),
            NEXT | RETURN,
            right.clone(),
            NEXT | CONTINUE,
        );
        assert_eq!(result.move_site(0, &[]), Some(site(1)));
        assert_eq!(result.move_site(1, &[]), Some(site(2)));
        let result = Ownership::join_continuing(entry, left, RETURN, right, RETURN);
        assert!(result.moved.is_empty());
    }

    #[test]
    fn joined_moves_and_loans_keep_first_path_provenance() {
        let mut left = Ownership::default();
        left.moved.insert(0, site(1));
        left.partial.insert(1, vec![(vec![0], site(2))]);
        left.loans.push(Loan {
            owner: 2,
            slice: 3,
            mutable: true,
            span: site(3),
        });
        let mut right = Ownership::default();
        right.moved.insert(0, site(4));
        right
            .partial
            .insert(1, vec![(vec![0], site(5)), (vec![1], site(6))]);
        right.loans.push(Loan {
            owner: 2,
            slice: 3,
            mutable: true,
            span: site(7),
        });
        let result = Ownership::join(left, right);
        assert_eq!(result.move_site(0, &[]), Some(site(1)));
        assert_eq!(result.move_site(1, &[0]), Some(site(2)));
        assert_eq!(result.move_site(1, &[1]), Some(site(6)));
        assert_eq!(result.loans.len(), 1);
        assert_eq!(result.conflicting_loan(2, false).unwrap().span, site(3));
    }

    #[test]
    fn shared_and_exclusive_access_obey_the_same_lexical_loan_rules() {
        let mut state = Ownership::default();
        state.loans.push(Loan {
            owner: 0,
            slice: 1,
            mutable: false,
            span: site(1),
        });
        state.loans.push(Loan {
            owner: 0,
            slice: 2,
            mutable: false,
            span: site(2),
        });
        assert!(state.conflicting_loan(0, false).is_none());
        assert_eq!(state.conflicting_loan(0, true).unwrap().span, site(1));
        assert!(state.conflicting_loan(3, true).is_none());
        state.loans.push(Loan {
            owner: 0,
            slice: 4,
            mutable: true,
            span: site(4),
        });
        assert_eq!(state.conflicting_loan(0, false).unwrap().span, site(4));
    }

    #[test]
    fn loop_and_protected_capture_guards_do_not_depend_on_parameter_layout() {
        let mut state = State::new();
        let outer = declare(&mut state, "outer", 0);
        state.loop_depth += 1;
        let iteration = declare(&mut state, "iteration", 1);
        assert!(!state.loop_move_allowed(outer));
        assert!(state.loop_move_allowed(iteration));
        state.tensor_body = Some((state.locals.len(), state.loop_depth));
        let protected = declare(&mut state, "protected", 2);
        assert!(!state.protected_move_allowed(outer));
        assert!(!state.protected_move_allowed(iteration));
        assert!(state.protected_move_allowed(protected));
        state.loop_depth += 1;
        assert!(!state.loop_move_allowed(protected));
        state.loop_depth -= 1;
        state.tensor_body = None;
        assert!(state.protected_move_allowed(outer));
    }
}
