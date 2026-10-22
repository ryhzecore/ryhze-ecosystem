#include "reference.hpp"
#include <charconv>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

namespace {
constexpr std::size_t input_limit = 1024 * 1024;
constexpr std::size_t line_limit = 256 * 1024;
constexpr std::size_t fixture_limit = 512;
constexpr std::size_t fixture_elements = 4096;
constexpr std::size_t fixture_rank = 4;
constexpr std::size_t fixture_transforms = 16;

struct TransportError : std::exception {};
struct BudgetInvariantError : std::exception {};

bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

struct Parser {
    std::string_view line;
    std::size_t position = 0;

    std::string_view token() {
        while (position < line.size() && whitespace(line[position])) ++position;
        const std::size_t start = position;
        while (position < line.size() && !whitespace(line[position])) ++position;
        if (start == position) throw TransportError();
        return line.substr(start, position - start);
    }

    void finish() {
        while (position < line.size() && whitespace(line[position])) ++position;
        if (position != line.size()) throw TransportError();
    }

    std::size_t number() {
        const auto text = token();
        for (char c : text) if (c < '0' || c > '9') throw TransportError();
        std::size_t value;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) throw TransportError();
        return value;
    }

    std::uint64_t raw_bits() {
        const auto text = token();
        if (text.size() != 16) throw TransportError();
        for (char c : text) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            throw TransportError();
        }
        std::uint64_t value;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) throw TransportError();
        return value;
    }
};

struct Transform {
    bool permutation;
    std::vector<std::size_t> values;
};

struct InputTensor {
    std::vector<std::size_t> shape;
    std::vector<std::uint64_t> values;
    std::vector<Transform> transforms;

    reference::Tensor materialize(reference::Budget& budget) const {
        return reference::Tensor::from_raw(budget, reference::Shape(shape), values);
    }

    reference::View transform(reference::View view) const {
        for (const auto& transform : transforms) {
            if (transform.permutation) view.permute(transform.values);
            else view.slice(transform.values[0], transform.values[1],
                              transform.values[2], transform.values[3]);
        }
        return view;
    }
};

InputTensor input_tensor(Parser& parser) {
    InputTensor input;
    const std::size_t rank = parser.number();
    if (rank > fixture_rank) throw TransportError();
    for (std::size_t axis = 0; axis < rank; ++axis) input.shape.push_back(parser.number());
    const std::size_t count = parser.number();
    if (count > fixture_elements) throw TransportError();
    for (std::size_t index = 0; index < count; ++index) input.values.push_back(parser.raw_bits());
    const std::size_t transform_count = parser.number();
    if (transform_count > fixture_transforms) throw TransportError();
    for (std::size_t index = 0; index < transform_count; ++index) {
        const auto kind = parser.token();
        Transform transform;
        if (kind == "p") {
            transform.permutation = true;
            const std::size_t permutation_rank = parser.number();
            if (permutation_rank > fixture_rank) throw TransportError();
            for (std::size_t axis = 0; axis < permutation_rank; ++axis) {
                transform.values.push_back(parser.number());
            }
        } else if (kind == "s") {
            transform.permutation = false;
            for (std::size_t parameter = 0; parameter < 4; ++parameter) {
                transform.values.push_back(parser.number());
            }
        } else throw TransportError();
        input.transforms.push_back(std::move(transform));
    }
    return input;
}

void execute(std::string_view line) {
    Parser parser{line};
    if (parser.token() != "v1") throw TransportError();
    const auto id = parser.token();
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_')) throw TransportError();
    }
    const auto operation = parser.token();
    const bool binary = operation == "add" || operation == "mul" || operation == "matmul";
    if (!binary && operation != "sum" && operation != "sigmoid"
        && operation != "softmax" && operation != "logsumexp") throw TransportError();
    const InputTensor a = input_tensor(parser);
    const std::size_t present = parser.number();
    if (present > 1 || (present == 1) != binary) throw TransportError();
    InputTensor b;
    if (present == 1) b = input_tensor(parser);
    parser.finish();
    reference::Budget budget(32 * 1024 * 1024);
    try {
        [&] {
        const auto left_owner = a.materialize(budget);
        const auto left = a.transform(left_owner.view());
        if (operation == "sum" || operation == "logsumexp") {
            const double scalar = operation == "sum" ? reference::sum(left) : reference::logsumexp(left);
            std::ostringstream rendered;
            rendered << "v1 " << id << " ok 0 1 " << std::hex << std::setfill('0') << std::setw(16)
                     << reference::bits_of(scalar);
            std::cout << rendered.str() << '\n';
            return;
        }
        reference::Tensor output;
        if (binary) {
            const auto right_owner = b.materialize(budget);
            const auto right = b.transform(right_owner.view());
            output = operation == "matmul" ? reference::matmul(budget, left, right)
                : reference::binary(budget, left, right, operation == "mul");
        } else if (operation == "sigmoid") output = reference::sigmoid(budget, left);
        else output = reference::softmax(budget, left);
        std::ostringstream rendered;
        rendered << "v1 " << id << " ok " << output.shape.size();
        for (std::size_t dimension : output.shape) rendered << ' ' << dimension;
        rendered << ' ' << output.count;
        for (std::size_t index = 0; index < output.count; ++index) {
            rendered << ' ' << std::hex << std::setfill('0') << std::setw(16)
                     << reference::bits_of(output.get(index));
        }
        std::cout << rendered.str() << '\n';
        }();
    } catch (const reference::Error& error) {
        std::cout << "v1 " << id << " err " << error.name << '\n';
    } catch (const std::bad_alloc&) {
        std::cout << "v1 " << id << " err AllocationFailure\n";
    }
    if (budget.used != 0) throw BudgetInvariantError();
}
} // namespace

int main() {
#ifdef _WIN32
    // Preserve the byte protocol before any iostream operation.
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) {
        return 2;
    }
#endif
    try {
        std::string input;
        input.reserve(input_limit);
        char value;
        while (std::cin.get(value)) {
            if (input.size() == input_limit) throw TransportError();
            input.push_back(value);
        }
        if (std::cin.bad()) throw TransportError();
        std::size_t start = 0;
        std::size_t fixtures = 0;
        while (start < input.size()) {
            const std::size_t newline = input.find('\n', start);
            const std::size_t end = newline == std::string::npos ? input.size() : newline;
            if (end - start > line_limit || ++fixtures > fixture_limit) throw TransportError();
            execute(std::string_view(input).substr(start, end - start));
            start = end + 1;
        }
        if (!std::cout) return 2;
    } catch (const TransportError&) {
        std::cerr << "invalid or oversized v1 fixture transport\n";
        return 2;
    } catch (const BudgetInvariantError&) {
        std::cerr << "tensor budget was not reclaimed after fixture\n";
        return 3;
    } catch (const std::bad_alloc&) {
        std::cerr << "fixture transport allocation failed\n";
        return 2;
    }
    return 0;
}
