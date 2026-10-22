#include "reference.hpp"
#include <iostream>
#include <type_traits>

static_assert(!std::is_copy_constructible<reference::Tensor>::value, "owners cannot copy");
static_assert(std::is_nothrow_move_constructible<reference::Tensor>::value, "owners move without allocation");

namespace {
std::size_t checks = 0;

void require(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
    ++checks;
}

template<class F> void error(const char* expected, F operation) {
    try {
        operation();
    } catch (const reference::Error& observed) {
        require(std::string(observed.name) == expected, "wrong error");
        return;
    }
    throw std::runtime_error("expected error missing");
}

reference::Tensor tensor(reference::Budget& budget,
                         std::vector<std::size_t> shape,
                         std::initializer_list<double> values) {
    std::vector<std::uint64_t> raw;
    for (double value : values) raw.push_back(reference::bits_of(value));
    return reference::Tensor::from_raw(budget, reference::Shape(shape), raw);
}
} // namespace

int main() {
    try {
        using namespace reference;
        Budget budget(max_budget_bytes);
        require(element_count({}) == 1, "scalar count");
        require(element_count({65536, 65536, 0, 65536}) == 0, "empty shape shortcut");
        error("RankLimit", [] { element_count(std::vector<std::size_t>(9, 0)); });
        error("ExtentLimit", [] { element_count({0, 65537}); });
        error("ElementLimit", [] { element_count({257, 256}); });
        error("BudgetLimit", [] { Budget invalid(max_budget_bytes + 1); });
        error("ShapeMismatch", [&] { tensor(budget, {2}, {1.0}); });
        require(bits_of(from_bits(UINT64_C(0xfff0000000000001))) == canonical_nan,
                "canonical signaling NaN ingress");
        require(bits_of(from_bits(UINT64_C(0x8000000000000000))) == UINT64_C(0x8000000000000000),
                "signed zero ingress");
        auto empty_owner = Tensor::allocate(budget, {65536, 0, 65536});
        auto empty = empty_owner.view();
        require(std::all_of(empty.strides.begin(), empty.strides.end(), [](auto value) { return value == 0; }), "empty strides");
        empty.slice(0, 65536, 65536, std::numeric_limits<std::size_t>::max());
        require(empty.offset == 0 && empty.count == 0, "empty slice canonical offset");
        error("Bounds", [&] { empty.slice(9, 0, 0, 0); });
        error("InvalidStep", [&] { empty.slice(0, 0, 0, 0); });
        auto view_owner = tensor(budget, {2, 3}, {1, 2, 3, 4, 5, 6});
        auto view = view_owner.view();
        view.permute({1, 0});
        require(view.get(1) == 4 && view.get(4) == 3, "transposed logical access");
        view.slice(0, 0, 3, 2);
        require(view.shape == Shape({2, 2})
                && view.get(2) == 3 && view.get(3) == 6, "strided slice");
        error("InvalidPermutation", [&] { view.permute({0, 0}); });
        error("Bounds", [&] { static_cast<void>(view.get(4)); });
        view.slice(0, 1, 2, std::numeric_limits<std::size_t>::max());
        require(view.get(0) == 3 && view.get(1) == 6, "singleton huge positive step");
        auto cancellation = tensor(budget, {3}, {9007199254740992.0, 1.0, -9007199254740992.0});
        require(bits_of(sum(cancellation.view())) == 0, "strict ordered sum");
        require(bits_of(sum(empty)) == 0, "empty sum starts positive zero");
        auto nan = Tensor::from_raw(budget, {}, {UINT64_C(0xfff0000000000001)});
        auto one = tensor(budget, {}, {1});
        require(bits_of(binary(budget, nan.view(), one.view(), false).get(0)) == canonical_nan,
                "public NaN addition result");
        require(bits_of(sum(nan.view())) == canonical_nan, "public NaN sum result");
        auto a = tensor(budget, {2, 2}, {1, 2, 3, 4});
        auto b = tensor(budget, {2, 1}, {2, 3});
        auto product = matmul(budget, a.view(), b.view());
        require(product.get(0) == 8 && product.get(1) == 18, "matrix product");
        auto no_inner_a = Tensor::allocate(budget, {2, 0});
        auto no_inner_b = Tensor::allocate(budget, {0, 3});
        auto no_inner_product = matmul(budget, no_inner_a.view(), no_inner_b.view());
        require(no_inner_product.count == 6 && bits_of(no_inner_product.get(5)) == 0,
                "zero inner dimension");
        error("RankMismatch", [&] { matmul(budget, one.view(), b.view()); });
        error("ShapeMismatch", [&] { matmul(budget, b.view(), b.view()); });
        auto work_a = Tensor::allocate(budget, {256, 16});
        auto work_b = Tensor::allocate(budget, {16, 256});
        const auto used_before = budget.used;
        error("WorkLimit", [&] { matmul(budget, work_a.view(), work_b.view()); });
        require(budget.used == used_before, "work preflight before allocation");
        auto edge = tensor(budget, {3}, {-1024, -0.0, 1024});
        auto sig = sigmoid(budget, edge.view());
        require(bits_of(sig.get(0)) == 0 && sig.get(1) == 0.5 && sig.get(2) == 1,
                "stable sigmoid extremes");
        auto negative_zero = tensor(budget, {1}, {-0.0});
        require(bits_of(logsumexp(negative_zero.view())) == UINT64_C(0x8000000000000000),
                "singleton logsumexp identity");
        auto equal = tensor(budget, {2}, {1000, 1000});
        auto probabilities = softmax(budget, equal.view());
        require(probabilities.get(0) == 0.5 && probabilities.get(1) == 0.5,
                "stable softmax equal large input");
        require(logsumexp(equal.view()) == 1000 + std::log(2.0), "stable logsumexp large input");
        error("NonFiniteInput", [&] { sigmoid(budget, nan.view()); });
        error("RankMismatch", [&] { softmax(budget, one.view()); });
        auto empty_vector = Tensor::allocate(budget, {0});
        error("EmptyInput", [&] { logsumexp(empty_vector.view()); });
        Budget tiny_budget(8);
        {
            auto input = tensor(tiny_budget, {}, {1});
            error("BudgetLimit", [&] { binary(tiny_budget, input.view(), input.view(), false); });
            require(sum(input.view()) == 1 && tiny_budget.used == 8, "scalar sum does not allocate");
            auto different_budget_result = binary(budget, input.view(), input.view(), false);
            require(different_budget_result.get(0) == 2 && tiny_budget.used == 8,
                    "explicit output allocator independent of input");
        }
        require(tiny_budget.used == 0, "budget reclaimed on scope exit");
        {
            auto original = tensor(tiny_budget, {}, {3});
            const auto identity = original.identity;
            auto moved = std::move(original);
            require(moved.identity == identity && tiny_budget.used == 8,
                    "move construction preserves identity and one charge");
            Budget replacement_budget(8);
            {
                auto replacement = tensor(replacement_budget, {}, {9});
                replacement = std::move(moved);
                require(replacement.identity == identity && replacement.get(0) == 3
                        && replacement_budget.used == 0 && tiny_budget.used == 8,
                        "move assignment releases previous payload");
            }
            require(tiny_budget.used == 0, "moved ownership charges released exactly once");
        }
        std::cout << "C++ reference selftest: " << checks << " checks passed\n";
    } catch (const std::exception& failure) {
        std::cerr << "C++ reference selftest failed: " << failure.what() << '\n';
        return 1;
    }
    return 0;
}
