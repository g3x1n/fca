#include "fca.hpp"

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

namespace {

using namespace fca;
using Clock = std::chrono::steady_clock;

struct Variant {
    std::string_view name;
    Method method;
};

constexpr std::array variants{
    Variant{"fca_unified", Method::unified},
    Variant{"dense", Method::dense},
    Variant{"polynomial", Method::polynomial},
};
constexpr std::array sizes_gib{1, 4, 16, 64, 256};
constexpr std::uint64_t gib = std::uint64_t{1} << 30;
constexpr std::uint32_t workers = 8;
constexpr std::size_t patterns = 8;

void tag_sweep() {
    const Params params;
    const auto block_count = [&params](int size_gib) {
        return std::uint32_t((size_gib * gib + params.block_bytes() - 1) / params.block_bytes());
    };
    const auto largest = block_count(sizes_gib.back());
    const Metadata meta{params, Id{1}, {}, largest * std::uint64_t(params.block_bytes()),
                        largest, 688};
    const Key key{42};
    std::array<std::vector<Block>, workers> buffers;
    std::array<std::array<Word, patterns>, workers> first_words;
    for (std::uint32_t worker = 0; worker < workers; ++worker) {
        std::mt19937_64 random(179 + worker);
        for (std::size_t pattern = 0; pattern < patterns; ++pattern) {
            auto& block = buffers[worker].emplace_back(params.block_words());
            for (auto& word : block) word = random();
            first_words[worker][pattern] = block.front();
        }
    }
    std::vector<Owner> owners;
    for (const auto& variant : variants) owners.emplace_back(key, meta, variant.method);

    std::array<double, variants.size()> elapsed{};
    std::array<Word, variants.size()> checksums{};
    std::uint32_t first = 0;
    std::cout << "target_gib,bytes,blocks,variant,workers,seconds,tag_MB_per_s,checksum\n";
    for (const auto size : sizes_gib) {
        const auto end = block_count(size);
        const auto bytes = end * std::uint64_t(params.block_bytes());
        for (std::size_t v = 0; v < variants.size(); ++v) {
            std::cerr << "TagGen " << variants[v].name << " through " << size << " GiB\n";
            const auto& owner = owners[v];
            std::array<Word, workers> worker_checksums{};
            const auto start = Clock::now();
            {
                std::vector<std::jthread> threads;
                threads.reserve(workers);
                for (std::uint32_t worker = 0; worker < workers; ++worker) {
                    threads.emplace_back([&, worker] {
                        auto& blocks = buffers[worker];
                        Word digest = 0;
                        for (std::uint32_t i = first + worker; i < end; i += workers) {
                            const auto pattern = (i / workers) % patterns;
                            auto& block = blocks[pattern];
                            block.front() = first_words[worker][pattern] ^ i;
                            digest ^= owner.tag(i, block).front();
                        }
                        worker_checksums[worker] = digest;
                    });
                }
            }
            elapsed[v] += std::chrono::duration<double>(Clock::now() - start).count();
            for (const auto checksum : worker_checksums) checksums[v] ^= checksum;
            std::cout << size << ',' << bytes << ',' << end << ',' << variants[v].name << ','
                      << workers << ',' << std::fixed << std::setprecision(6) << elapsed[v]
                      << ',' << (double(bytes) / 1e6 / elapsed[v]) << ','
                      << std::hex << checksums[v] << std::dec << '\n' << std::flush;
        }
        first = end;
    }
}

void scope_state() {
    const Params params;
    const Key key{42};
    Metadata scoped{params, Id{}, Id{7}, params.block_bytes(), 1, 1};
    const auto shared = std::make_shared<const Authenticator>(key, scoped);
    Block block(params.block_words(), 0);
    block.front() = 0x1234abcd;
    const auto expected = shared->evaluate(block);

    std::vector<Owner> owners;
    std::vector<Authenticator> dense;
    std::vector<Authenticator> polynomial;
    constexpr std::array checkpoints{1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    owners.reserve(checkpoints.back());
    dense.reserve(checkpoints.back());
    polynomial.reserve(checkpoints.back());
    std::size_t dense_elements = 0;
    std::size_t polynomial_elements = 0;
    std::cout << "versions,variant,cache_objects,cache_elements,cache_bytes\n";
    unsigned first = 0;
    for (const unsigned end : checkpoints) {
        for (unsigned version = first; version < end; ++version) {
            scoped.file = Id{static_cast<std::uint8_t>(version),
                             static_cast<std::uint8_t>(version >> 8)};
            owners.emplace_back(key, scoped, Method::unified, shared);
            if (owners.back().evaluate(block) != expected) {
                throw std::runtime_error("factor reuse changed the authentication value");
            }
            Metadata standalone = scoped;
            standalone.scope.reset();
            dense.emplace_back(key, standalone, Method::dense);
            polynomial.emplace_back(key, standalone, Method::polynomial);
            dense_elements += dense.back().elements();
            polynomial_elements += polynomial.back().elements();
        }
        std::cout << end << ",fca_unified,1," << shared->elements() << ','
                  << shared->elements() * sizeof(Field) << '\n'
                  << end << ",dense," << dense.size() << ',' << dense_elements << ','
                  << dense_elements * sizeof(Field) << '\n'
                  << end << ",polynomial," << polynomial.size() << ','
                  << polynomial_elements << ',' << polynomial_elements * sizeof(Field)
                  << '\n' << std::flush;
        first = end;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view experiment = argv[1];
    if (experiment == "tag-sweep") tag_sweep();
    else if (experiment == "scope-state") scope_state();
    else return 2;
}
