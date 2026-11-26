#include "local.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {

using namespace fca;
using Clock = std::chrono::steady_clock;

struct Variant {
    std::string_view name;
    Method method;
    std::uint32_t branches;
    bool explicit_request;
};

constexpr Variant variants[] = {
    {"fca_column_tail", Method::column_tail, 1, false},
    {"fca_unified", Method::unified, 1, false},
    {"fca_on_the_fly", Method::on_the_fly, 1, false},
    {"fca_t2", Method::unified, 2, false},
    {"dense", Method::dense, 1, true},
    {"polynomial", Method::polynomial, 1, true},
};

struct Case {
    const Variant& variant;
    std::vector<Field> tags;
    std::vector<double> tag_rates;
};

double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint32_t number(std::string_view value) {
    std::uint32_t result;
    const auto end = value.data() + value.size();
    auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result == 0) {
        throw std::invalid_argument("expected a positive integer");
    }
    return result;
}

void fixture(const fs::path& path, const Metadata& meta) {
    if (fs::exists(path)) throw std::runtime_error("fixture already exists");
    std::ofstream out(path, std::ios::binary);
    std::mt19937_64 random(0x464341ULL);
    Block block(meta.params.block_words());
    for (std::uint32_t i = 0; i < meta.count; ++i) {
        std::ranges::generate(block, [&random] { return random(); });
        if constexpr (std::endian::native == std::endian::little) {
            write_bytes(out, {reinterpret_cast<const std::uint8_t*>(block.data()),
                              meta.params.block_bytes()});
        } else {
            write_bytes(out, serialize_block(meta.params, block));
        }
    }
    out.close();
    if (!out) throw std::runtime_error("fixture write failed");
}

void run(const fs::path& path, std::uint32_t blocks, std::uint32_t trials,
         std::uint32_t tag_batch, const std::vector<std::uint32_t>& sample_sizes) {
    const Params params;
    Metadata meta{params, {}, {}, std::uint64_t(blocks) * params.block_bytes(),
                  blocks, sample_sizes.front()};
    meta.validate();
    if (tag_batch > blocks || *std::ranges::max_element(sample_sizes) > blocks) {
        throw std::invalid_argument("tag batch and sample sizes cannot exceed block count");
    }
    fixture(path, meta);
    BlockReader reader(path, meta);
    const Algebra algebra(params);

    // Fixed keys and seeds are used only to make this synthetic benchmark repeatable.
    Key key{};
    key[0] = 0x46;
    std::vector<Case> cases;
    for (const auto& variant : variants) cases.push_back({variant, std::vector<Field>(blocks), {}});
    std::vector<Block> batch;
    batch.reserve(tag_batch);
    for (std::uint32_t i = 0; i < tag_batch; ++i) batch.push_back(reader.read(i));

    for (std::uint32_t trial = 0; trial < trials; ++trial) {
        for (std::size_t j = 0; j < cases.size(); ++j) {
            auto& item = cases[(j + trial) % cases.size()];
            auto variant_meta = meta;
            variant_meta.params.branches = item.variant.branches;
            const Owner owner(key, variant_meta, item.variant.method);
            const auto start = Clock::now();
            for (std::uint32_t i = 0; i < tag_batch; ++i) item.tags[i] = owner.tag(i, batch[i]);
            item.tag_rates.push_back((tag_batch * double(params.block_bytes()) / 1000) / ms(start));
        }
    }
    // One untimed pass for tags outside the timed batch. Read each source block once.
    std::vector<Owner> owners;
    for (const auto& item : cases) {
        auto variant_meta = meta;
        variant_meta.params.branches = item.variant.branches;
        owners.emplace_back(key, variant_meta, item.variant.method);
    }
    for (std::uint32_t i = tag_batch; i < blocks; ++i) {
        const auto block = reader.read(i);
        for (std::size_t j = 0; j < cases.size(); ++j) cases[j].tags[i] = owners[j].tag(i, block);
    }

    std::cout << "variant,c,trial,tag_MB_per_s,read_ms,operator_ms,tag_aggregate_ms,proof_ms,"
                 "challenge_expand_ms,verify_eval_ms,mask_ms,verify_total_ms,valid,"
                 "cache_elements,cache_packed_bytes,cache_24byte_bytes,cache_memory_bytes,"
                 "request_bytes,response_bytes\n";
    std::cout << std::fixed << std::setprecision(6);
    for (const auto c : sample_sizes) {
        meta.samples = c;
        std::vector<Owner> verifiers;
        std::vector<std::size_t> elements;
        for (const auto& item : cases) {
            auto variant_meta = meta;
            variant_meta.params.branches = item.variant.branches;
            const auto auth = std::make_shared<const Authenticator>(key, variant_meta, item.variant.method);
            verifiers.emplace_back(key, variant_meta, item.variant.method, auth);
            elements.push_back(auth->elements());
        }
        for (std::uint32_t trial = 0; trial < trials; ++trial) {
            Challenge challenge{meta.file, std::uint64_t(c) << 32 | trial, {}};
            challenge.seed[0] = 0xa5;
            for (unsigned i = 0; i < 4; ++i) challenge.seed[1 + i] = trial >> (8 * i);
            auto start = Clock::now();
            const auto terms = expand(meta, challenge);
            const auto expansion = ms(start);
            Block aggregate(params.block_words());
            double read_time = 0;
            double operator_time = 0;
            for (const auto& term : terms) {
                start = Clock::now();
                const auto block = reader.read(term.index);
                read_time += ms(start);
                start = Clock::now();
                algebra.add_scaled(aggregate, block, term.coefficient);
                operator_time += ms(start);
            }
            for (std::size_t offset = 0; offset < cases.size(); ++offset) {
                const std::size_t j = (offset + trial) % cases.size();
                const auto& item = cases[j];
                const auto& owner = verifiers[j];
                start = Clock::now();
                Field tag{};
                for (const auto& term : terms) {
                    tag ^= algebra.multiply(term.coefficient, item.tags[term.index]);
                }
                const auto tag_time = ms(start);

                start = Clock::now();
                const auto evaluation = owner.evaluate(aggregate);
                const auto eval_time = ms(start);
                start = Clock::now();
                const auto mask = owner.aggregate_mask(terms);
                const auto mask_time = ms(start);
                const bool ok = (evaluation ^ mask) == tag;
                if (!ok) throw std::runtime_error("benchmark proof failed verification");

                const auto request = item.variant.explicit_request
                    ? 24 + c * (std::size_t{4} + params.field_bytes())
                    : serialize_challenge(challenge).size();
                const auto response = 24 + params.block_bytes() + params.field_bytes();
                std::cout << item.variant.name << ',' << c << ',' << trial + 1 << ','
                          << item.tag_rates[trial] << ',' << read_time << ',' << operator_time << ','
                          << tag_time << ',' << read_time + operator_time + tag_time << ','
                          << expansion << ',' << eval_time << ',' << mask_time << ','
                          << eval_time + mask_time + (item.variant.explicit_request ? 0 : expansion)
                          << ',' << ok << ','
                          << elements[j] << ',' << elements[j] * params.field_bytes() << ','
                          << elements[j] * 24 << ',' << elements[j] * sizeof(Field) << ','
                          << request << ',' << response << '\n';
            }
            std::cout.flush();
            std::cerr << "c=" << c << ", trial " << trial + 1 << '/' << trials << '\n';
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 6) {
            std::cerr << "usage: fca-bench NEW_FIXTURE BLOCKS TRIALS TAG_BATCH C [C...]\n";
            return 2;
        }
        std::vector<std::uint32_t> sample_sizes;
        for (int i = 5; i < argc; ++i) sample_sizes.push_back(number(argv[i]));
        run(argv[1], number(argv[2]), number(argv[3]), number(argv[4]), sample_sizes);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
