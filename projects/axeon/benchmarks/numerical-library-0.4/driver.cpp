#include "kernel.hpp"
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {
constexpr std::uint64_t hash_offset = UINT64_C(0xcbf29ce484222325);
constexpr std::uint64_t hash_prime = UINT64_C(0x100000001b3);
std::size_t number(std::string_view text) {
    if (text.empty()) throw std::runtime_error("missing integer");
    for (char c : text) if (c < '0' || c > '9') throw std::runtime_error("invalid integer");
    std::size_t value;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::runtime_error("invalid integer");
    return value;
}
std::uint64_t bits(std::string_view text) {
    if (text.size() != 16) throw std::runtime_error("invalid hex word");
    for (char c : text) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) throw std::runtime_error("invalid hex word");
    std::uint64_t value;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (parsed.ec != std::errc{}) throw std::runtime_error("invalid hex word");
    return value;
}
std::string hex(std::uint64_t value) {
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << value;
    return result.str();
}
std::uint64_t word_bits(double value) {
    // Tensor stores are already canonical. Match Rust f64::to_bits exactly.
    std::uint64_t word;
    std::memcpy(&word, &value, sizeof word);
    return word;
}
std::size_t image_rss() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            std::istringstream fields(line.substr(6));
            std::size_t value = 0;
            std::string units, extra;
            if (!(fields >> value >> units) || value == 0 || units != "kB" || (fields >> extra)) {
                throw std::runtime_error("VmHWM malformed");
            }
            return value;
        }
    }
    throw std::runtime_error("VmHWM unavailable");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 6) throw std::runtime_error("usage: driver check|measure input-file dimension batch checksum");
        const std::string mode(argv[1]);
        if (mode != "check" && mode != "measure") throw std::runtime_error("invalid mode");
        const auto n = number(argv[3]), batch = number(argv[4]);
        if ((n != 8 && n != 16) || batch != 16) throw std::runtime_error("unsupported dimension or batch");
        const auto expected_hash = bits(argv[5]);
        std::ifstream source(argv[2], std::ios::binary);
        if (!source) throw std::runtime_error("input unavailable");
        std::string text;
        char character;
        while (source.get(character)) {
            if (text.size() == 16 * 1024) throw std::runtime_error("oversized input");
            text.push_back(character);
        }
        if (source.bad()) throw std::runtime_error("input read failed");
        std::istringstream tokens(text);
        auto token = [&]() { std::string value; if (!(tokens >> value)) throw std::runtime_error("truncated input"); return value; };
        if (token() != "AXNUMBENCH1" || number(token()) != n || number(token()) != n*n) throw std::runtime_error("input dimensions");
        std::vector<std::uint64_t> raw_a, raw_b;
        for (std::size_t i = 0; i < n*n; ++i) raw_a.push_back(bits(token()));
        for (std::size_t i = 0; i < n*n; ++i) raw_b.push_back(bits(token()));
        std::string extra;
        if (tokens >> extra) throw std::runtime_error("trailing input");
        reference::Budget budget(1024 * 1024);
        {
            const auto a = reference::Tensor::from_raw(budget, reference::Shape{n,n}, raw_a);
            const auto b = reference::Tensor::from_raw(budget, reference::Shape{n,n}, raw_b);
            const auto left = a.view(), right = b.view();
            const std::size_t baseline = 2*n*n*sizeof(double);
            if (budget.used != baseline) throw std::runtime_error("input budget mismatch");
            if (mode == "check") {
                {
                    const auto output = checked_matmul(budget, left, right);
                    std::uint64_t checksum = hash_offset;
                    std::cout << "{\"dimension\":" << n << ",\"bits\":[";
                    for (std::size_t i = 0; i < output.count; ++i) {
                        const auto word = word_bits(output.get(i));
                        checksum = (checksum ^ word) * hash_prime;
                        if (i != 0) std::cout << ',';
                        std::cout << '"' << hex(word) << '"';
                    }
                    std::cout << "],\"checksum\":\"" << hex(checksum) << "\"}\n";
                }
                if (budget.used != baseline) throw std::runtime_error("output budget not reclaimed");
            } else {
                static_assert(std::chrono::steady_clock::is_steady, "monotonic clock required");
                std::array<std::uint64_t, 9> durations{}, checksums{};
                for (std::size_t call = 0; call < 9; ++call) {
                    const auto start = std::chrono::steady_clock::now();
                    std::uint64_t checksum = hash_offset;
                    for (std::size_t iteration = 0; iteration < batch; ++iteration) {
                        const auto output = checked_matmul(budget, left, right);
                        for (std::size_t i = 0; i < output.count; ++i) {
                            checksum = (checksum ^ word_bits(output.get(i))) * hash_prime;
                        }
                    }
                    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start).count();
                    if (checksum != expected_hash || budget.used != baseline) throw std::runtime_error("timed checksum or budget mismatch");
                    if (elapsed <= 0) throw std::runtime_error("nonpositive elapsed time");
                    durations[call] = static_cast<std::uint64_t>(elapsed);
                    checksums[call] = checksum;
                }
                for (std::size_t call = 0; call < 9; ++call) {
                    std::cout << "{\"call\":" << call << ",\"elapsed_ns\":" << durations[call]
                              << ",\"checksum\":\"" << hex(checksums[call]) << "\",\"batch\":" << batch << "}\n";
                }
            }
        }
        if (budget.used != 0) throw std::runtime_error("owner budget not reclaimed");
        std::cout << "{\"image_peak_rss_kib\":" << image_rss() << ",\"final_budget_bytes\":0}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
