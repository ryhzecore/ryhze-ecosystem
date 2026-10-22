#ifndef AXEON_NUMERICS_TEST_REFERENCE_HPP
#define AXEON_NUMERICS_TEST_REFERENCE_HPP

// Independently authored checked C++ comparator, not a library implementation
// used by Axeon. Metadata is inline, storage is move-only and budgets are borrowed.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace reference {
constexpr std::size_t max_rank = 8;
constexpr std::size_t max_extent = 65536;
constexpr std::size_t max_elements = 65536;
constexpr std::size_t max_matmul_work = 1048576;
constexpr std::size_t max_budget_bytes = 64 * 1024 * 1024;
constexpr std::uint64_t canonical_nan = UINT64_C(0x7ff8000000000000);

struct Error : std::exception {
    const char* name;
    explicit Error(const char* value) : name(value) {}
    const char* what() const noexcept override { return name; }
};

inline std::uint64_t canonical_bits(std::uint64_t bits) {
    if ((bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000)
        && (bits & UINT64_C(0x000fffffffffffff)) != 0) return canonical_nan;
    return bits;
}
inline double from_bits(std::uint64_t bits) {
    bits = canonical_bits(bits);
    double value;
    static_assert(sizeof value == sizeof bits && std::numeric_limits<double>::is_iec559,
                  "IEEE binary64 required");
    std::memcpy(&value, &bits, sizeof value);
    return value;
}
inline std::uint64_t bits_of(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return canonical_bits(bits);
}
inline double canonicalize(double value) { return from_bits(bits_of(value)); }
inline std::size_t checked_add(std::size_t a, std::size_t b, const char* error) {
    if (b > std::numeric_limits<std::size_t>::max() - a) throw Error(error);
    return a + b;
}
inline std::size_t checked_multiply(std::size_t a, std::size_t b, const char* error) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) throw Error(error);
    return a * b;
}

class Shape {
    std::array<std::size_t, max_rank> dimensions_{};
    std::size_t rank_ = 0;
    std::size_t count_ = 1;
public:
    std::array<std::size_t, max_rank> dense_strides{};
    Shape() = default;
    Shape(const std::size_t* dimensions, std::size_t rank) : rank_(rank) {
        if (rank > max_rank) throw Error("RankLimit");
        bool empty = false;
        for (std::size_t axis = 0; axis < rank; ++axis) {
            if (dimensions[axis] > max_extent) throw Error("ExtentLimit");
            dimensions_[axis] = dimensions[axis];
            empty = empty || dimensions[axis] == 0;
        }
        if (empty) { count_ = 0; return; }
        for (std::size_t axis = rank; axis != 0; --axis) {
            dense_strides[axis - 1] = count_;
            count_ = checked_multiply(count_, dimensions_[axis - 1], "ElementLimit");
            if (count_ > max_elements) throw Error("ElementLimit");
        }
    }
    Shape(std::initializer_list<std::size_t> dimensions)
        : Shape(dimensions.begin(), dimensions.size()) {}
    explicit Shape(const std::vector<std::size_t>& dimensions)
        : Shape(dimensions.data(), dimensions.size()) {}
    std::size_t count() const { return count_; }
    std::size_t size() const { return rank_; }
    std::size_t operator[](std::size_t axis) const {
        if (axis >= rank_) throw Error("Bounds");
        return dimensions_[axis];
    }
    const std::size_t* begin() const { return dimensions_.data(); }
    const std::size_t* end() const { return dimensions_.data() + rank_; }
    bool operator==(const Shape& other) const {
        return rank_ == other.rank_ && std::equal(begin(), end(), other.begin());
    }
    bool operator!=(const Shape& other) const { return !(*this == other); }
};
inline std::size_t element_count(std::initializer_list<std::size_t> dimensions) {
    return Shape(dimensions).count();
}
inline std::size_t element_count(const std::vector<std::size_t>& dimensions) {
    return Shape(dimensions).count();
}

struct Budget {
    std::size_t limit;
    std::size_t used = 0;
    explicit Budget(std::size_t bytes) : limit(bytes) {
        if (bytes > max_budget_bytes) throw Error("BudgetLimit");
    }
    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;
    void reserve(std::size_t count) {
        const std::size_t bytes = checked_multiply(count, sizeof(double), "BudgetLimit");
        if (bytes > limit - used) throw Error("BudgetLimit");
        used += bytes;
    }
    void release(std::size_t count) noexcept { used -= count * sizeof(double); }
};

class View;
class Tensor {
    Budget* budget_ = nullptr;
    std::vector<double> values_;
    inline static std::atomic<std::uint64_t> next_identity_{1};
    void relinquish() noexcept {
        if (budget_ != nullptr) budget_->release(count);
        budget_ = nullptr;
    }
public:
    Shape shape;
    std::size_t count = 0;
    std::uint64_t identity = 0;
    Tensor() = default;
    Tensor(const Tensor&) = delete;
    Tensor& operator=(const Tensor&) = delete;
    Tensor(Tensor&& source) noexcept
        : budget_(std::exchange(source.budget_, nullptr)), values_(std::move(source.values_)),
          shape(source.shape), count(source.count), identity(source.identity) {}
    Tensor& operator=(Tensor&& source) noexcept {
        if (this != &source) {
            relinquish();
            budget_ = std::exchange(source.budget_, nullptr);
            values_ = std::move(source.values_);
            shape = source.shape;
            count = source.count;
            identity = source.identity;
        }
        return *this;
    }
    ~Tensor() { relinquish(); }
    static Tensor allocate(Budget& budget, const Shape& dimensions) {
        Tensor result;
        result.shape = dimensions;
        result.count = dimensions.count();
        budget.reserve(result.count);
        result.budget_ = &budget;
        try { result.values_.resize(result.count, 0.0); }
        catch (const std::bad_alloc&) { throw Error("AllocationFailure"); }
        auto current = next_identity_.load(std::memory_order_relaxed);
        for (;;) {
            if (current == std::numeric_limits<std::uint64_t>::max()) throw Error("AllocationFailure");
            if (next_identity_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                result.identity = current;
                break;
            }
        }
        return result;
    }
    static Tensor from_raw(Budget& budget, const Shape& dimensions,
                           const std::vector<std::uint64_t>& raw) {
        if (dimensions.count() != raw.size()) throw Error("ShapeMismatch");
        Tensor result = allocate(budget, dimensions);
        for (std::size_t index = 0; index < raw.size(); ++index) result.values_[index] = from_bits(raw[index]);
        return result;
    }
    double get(std::size_t index) const {
        if (index >= values_.size()) throw Error("Bounds");
        return values_[index];
    }
    void store(std::size_t index, double value) {
        if (index >= values_.size()) throw Error("Bounds");
        values_[index] = canonicalize(value);
    }
    View view() const;
};

class View {
    const double* data_ = nullptr;
    std::size_t storage_count_ = 0;
public:
    Shape shape;
    std::array<std::size_t, max_rank> strides{};
    std::size_t offset = 0;
    std::size_t count = 0;
    std::uint64_t identity = 0;
    View(const double* data, std::size_t storage_count, const Shape& dimensions,
         std::uint64_t allocation_identity)
        : data_(data), storage_count_(storage_count), shape(dimensions),
          strides(dimensions.dense_strides), count(dimensions.count()), identity(allocation_identity) {}
    std::size_t position(std::size_t logical) const {
        if (logical >= count) throw Error("Bounds");
        std::size_t position = offset;
        for (std::size_t axis = shape.size(); axis != 0; --axis) {
            const std::size_t coordinate = logical % shape[axis - 1];
            logical /= shape[axis - 1];
            position = checked_add(position,
                                   checked_multiply(coordinate, strides[axis - 1], "Bounds"), "Bounds");
        }
        if (position >= storage_count_) throw Error("Bounds");
        return position;
    }
    double get(std::size_t logical) const { return data_[position(logical)]; }
    void validate() {
        if (count == 0) { offset = 0; strides.fill(0); return; }
        std::size_t last = offset;
        for (std::size_t axis = 0; axis < shape.size(); ++axis) {
            last = checked_add(last, checked_multiply(shape[axis] - 1, strides[axis], "Bounds"), "Bounds");
        }
        if (last >= storage_count_) throw Error("Bounds");
    }
    void permute(const std::vector<std::size_t>& axes) {
        if (axes.size() != shape.size()) throw Error("InvalidPermutation");
        std::array<bool, max_rank> seen{};
        std::array<std::size_t, max_rank> dimensions{}, new_strides{};
        for (std::size_t axis = 0; axis < axes.size(); ++axis) {
            if (axes[axis] >= shape.size() || seen[axes[axis]]) throw Error("InvalidPermutation");
            seen[axes[axis]] = true;
            dimensions[axis] = shape[axes[axis]];
            new_strides[axis] = strides[axes[axis]];
        }
        shape = Shape(dimensions.data(), axes.size());
        strides = new_strides;
        validate();
    }
    void slice(std::size_t axis, std::size_t start, std::size_t end, std::size_t step) {
        if (axis >= shape.size() || start > end || end > shape[axis]) throw Error("Bounds");
        if (step == 0) throw Error("InvalidStep");
        const std::size_t distance = end - start;
        const std::size_t length = distance / step + (distance % step != 0);
        std::array<std::size_t, max_rank> dimensions{};
        std::copy(shape.begin(), shape.end(), dimensions.begin());
        dimensions[axis] = length;
        const Shape sliced(dimensions.data(), shape.size());
        if (sliced.count() == 0) {
            shape = sliced;
            count = 0;
            validate();
            return;
        }
        const std::size_t new_stride = length > 1
            ? checked_multiply(strides[axis], step, "InvalidStep") : strides[axis];
        offset = checked_add(offset, checked_multiply(start, strides[axis], "Bounds"), "Bounds");
        strides[axis] = new_stride;
        shape = sliced;
        count = sliced.count();
        validate();
    }
};
inline View Tensor::view() const { return View(values_.data(), values_.size(), shape, identity); }

inline Tensor binary(Budget& budget, const View& a, const View& b, bool multiply) {
    if (a.shape != b.shape) throw Error("ShapeMismatch");
    Tensor output = Tensor::allocate(budget, a.shape);
    for (std::size_t index = 0; index < a.count; ++index) {
        output.store(index, multiply ? a.get(index) * b.get(index) : a.get(index) + b.get(index));
    }
    return output;
}
inline double sum(const View& input) {
    double total = 0.0;
    for (std::size_t index = 0; index < input.count; ++index) total = total + input.get(index);
    return canonicalize(total);
}
inline Tensor matmul(Budget& budget, const View& a, const View& b) {
    if (a.shape.size() != 2 || b.shape.size() != 2) throw Error("RankMismatch");
    if (a.shape[1] != b.shape[0]) throw Error("ShapeMismatch");
    const std::size_t rows = a.shape[0], inner = a.shape[1], columns = b.shape[1];
    const Shape shape{rows, columns};
    const std::size_t output_count = shape.count();
    const std::size_t products = checked_multiply(output_count, inner, "WorkLimit");
    if (checked_add(products, output_count, "WorkLimit") > max_matmul_work) throw Error("WorkLimit");
    Tensor output = Tensor::allocate(budget, shape);
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            double total = 0.0;
            for (std::size_t k = 0; k < inner; ++k) {
                const double product = a.get(row * inner + k) * b.get(k * columns + column);
                total = total + product;
            }
            output.store(row * columns + column, total);
        }
    }
    return output;
}
inline void require_finite(const View& input) {
    for (std::size_t index = 0; index < input.count; ++index) {
        if (!std::isfinite(input.get(index))) throw Error("NonFiniteInput");
    }
}
inline double finite_result(double value) {
    if (!std::isfinite(value)) throw Error("NumericalFailure");
    return value;
}
inline double unit_interval(double value) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) throw Error("NumericalFailure");
    return value;
}
inline Tensor sigmoid(Budget& budget, const View& input) {
    require_finite(input);
    Tensor output = Tensor::allocate(budget, input.shape);
    for (std::size_t index = 0; index < input.count; ++index) {
        const double value = input.get(index);
        const double exponential = unit_interval(std::exp(value >= 0.0 ? -value : value));
        const double result = value >= 0.0 ? 1.0 / (1.0 + exponential) : exponential / (1.0 + exponential);
        output.store(index, unit_interval(result));
    }
    return output;
}
inline std::pair<std::size_t, double> first_maximum(const View& input) {
    std::size_t maximum_index = 0;
    double maximum = input.get(0);
    for (std::size_t index = 1; index < input.count; ++index) {
        const double value = input.get(index);
        if (value > maximum) { maximum_index = index; maximum = value; }
    }
    return {maximum_index, maximum};
}
inline void require_vector(const View& input) {
    if (input.shape.size() != 1) throw Error("RankMismatch");
    if (input.count == 0) throw Error("EmptyInput");
    require_finite(input);
}
inline Tensor softmax(Budget& budget, const View& input) {
    require_vector(input);
    const double maximum = first_maximum(input).second;
    Tensor output = Tensor::allocate(budget, input.shape);
    for (std::size_t index = 0; index < input.count; ++index) {
        output.store(index, unit_interval(std::exp(input.get(index) - maximum)));
    }
    double total = 0.0;
    for (std::size_t index = 0; index < input.count; ++index) total = total + output.get(index);
    if (!std::isfinite(total) || total < 1.0) throw Error("NumericalFailure");
    for (std::size_t index = 0; index < input.count; ++index) {
        output.store(index, unit_interval(output.get(index) / total));
    }
    return output;
}
inline double logsumexp(const View& input) {
    require_vector(input);
    const auto maximum = first_maximum(input);
    if (input.count == 1) return canonicalize(maximum.second);
    double tail = 0.0;
    for (std::size_t index = 0; index < input.count; ++index) {
        if (index != maximum.first) tail = tail + unit_interval(std::exp(input.get(index) - maximum.second));
    }
    if (!std::isfinite(tail) || tail < 0.0) throw Error("NumericalFailure");
    const double correction = std::log1p(tail);
    if (!std::isfinite(correction) || correction < 0.0) throw Error("NumericalFailure");
    return canonicalize(finite_result(maximum.second + correction));
}
} // namespace reference
#endif
