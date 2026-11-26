#include "fca.hpp"

#include <algorithm>
#include <bit>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>

namespace {

using namespace fca;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename F>
void rejects(F operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("noncanonical input was accepted");
}

bool bit(const Field& x, unsigned i) { return (x[i / 64] >> (i % 64)) & 1; }

void toggle(Field& x, unsigned i) { x[i / 64] ^= Word{1} << (i % 64); }

Field reference_multiply(const Field& a, const Field& b, unsigned n) {
    std::vector<bool> product(2 * n);
    for (unsigned i = 0; i < n; ++i) {
        if (!bit(a, i)) continue;
        for (unsigned j = 0; j < n; ++j) {
            if (bit(b, j)) product[i + j].flip();
        }
    }
    for (unsigned i = 2 * n - 1; i >= n; --i) {
        if (!product[i]) continue;
        for (unsigned j = i - n; j <= i; ++j) product[j].flip();
    }
    Field out{};
    for (unsigned i = 0; i < n; ++i) {
        if (product[i]) toggle(out, i);
    }
    return out;
}

Block random_block(const Params& params, std::mt19937_64& rng) {
    Block block(params.block_words());
    const Word mask = ~Word{0} >> (64 - params.width);
    std::ranges::generate(block, [&] { return rng() & mask; });
    return block;
}

Field random_field(const Params& params, std::mt19937_64& rng) {
    Field value{};
    for (unsigned i = 0; i < params.degree(); ++i) {
        if (rng() & 1) toggle(value, i);
    }
    return value;
}

std::vector<Field> reference_encode(const Params& p, const Block& block) {
    std::vector<Field> out(std::size_t(p.columns) * p.width);
    for (unsigned j = 0; j < p.columns; ++j) {
        for (unsigned r = 0; r < p.degree(); ++r) {
            for (unsigned k = 0; k < p.width; ++k) {
                if ((block[j * p.degree() + r] >> k) & 1) toggle(out[j * p.width + k], r);
            }
        }
    }
    return out;
}

Field reference_evaluate(const Params& p, const Block& block, const std::vector<Factors>& factors) {
    const auto encoded = reference_encode(p, block);
    const auto n = p.degree();
    Field out{};
    for (const auto& branch : factors) {
        for (unsigned j = 0; j < p.columns; ++j) {
            for (unsigned k = 0; k < p.width; ++k) {
                const auto term = reference_multiply(branch.columns[j], branch.planes[k], n);
                out ^= reference_multiply(term, encoded[j * p.width + k], n);
            }
        }
    }
    return out;
}

void algebra_test() {
    std::mt19937_64 rng(2026);
    for (const unsigned prime : {3, 5, 11, 179, 227}) {
        const Params p{prime, 3, 64, 2};
        const Algebra algebra(p);
        const auto n = p.degree();
        for (unsigned i = 0; i < 16; ++i) {
            const auto left = random_field(p, rng);
            const auto right = random_field(p, rng);
            check(algebra.multiply(left, right) == reference_multiply(left, right, n),
                  "field multiplication differs from polynomial long division");
        }
        const auto block = random_block(p, rng);
        const auto encoded = reference_encode(p, block);
        check(algebra.encode(block) == encoded, "word transpose differs from Eq. (2)");
        const auto scalar = random_field(p, rng);
        Block scaled(p.block_words());
        algebra.add_scaled(scaled, block, scalar);
        const auto scaled_fields = reference_encode(p, scaled);
        for (unsigned i = 0; i < encoded.size(); ++i) {
            check(scaled_fields[i] == reference_multiply(scalar, encoded[i], n),
                  "word circular convolution differs from field multiplication");
        }
        Metadata meta{p, {}, {}, p.block_bytes(), 1, 1};
        std::vector<Factors> factors(p.branches);
        for (auto& branch : factors) {
            for (unsigned j = 0; j < p.columns; ++j) branch.columns.push_back(random_field(p, rng));
            for (unsigned k = 0; k < p.width; ++k) branch.planes.push_back(random_field(p, rng));
        }
        const Authenticator auth(meta, factors);
        check(auth.evaluate(block) == reference_evaluate(p, block, factors),
              "FCA evaluation differs from Eq. (5)");
        check(parse_block(p, serialize_block(p, block)) == block, "block wire round trip");
        check(parse_field(p, serialize_field(p, scalar)) == scalar, "field wire round trip");
    }
    rejects([] { Params{7}.validate(); });
    rejects([] { Params{9}.validate(); });
    rejects([] { Params{179, 1, 65}.validate(); });
}

void rank_distribution_test() {
    const auto factors = [](unsigned code) {
        return Factors{{Field{code & 3}, Field{(code >> 2) & 3}},
                       {Field{(code >> 4) & 3}, Field{(code >> 6) & 3}}};
    };
    for (unsigned rank : {1, 2}) {
        // Matrices [1,2;2,3] and [1,2;2,1] have ranks 1 and 2 over F_4.
        const Block delta{1, 2, 2, rank == 1 ? 3U : 1U};
        for (unsigned t : {1, 2}) {
            const Params p{3, 2, 2, t};
            const Metadata meta{p, {}, {}, p.block_bytes(), 1, 1};
            std::array<unsigned, 4> histogram{};
            const unsigned combinations = t == 1 ? 256 : 65536;
            for (unsigned i = 0; i < combinations; ++i) {
                std::vector<Factors> branches{factors(i)};
                if (t == 2) branches.push_back(factors(i >> 8));
                const auto out = Authenticator(meta, std::move(branches)).evaluate(delta);
                check(out[0] < histogram.size(), "F_4 output outside field");
                ++histogram[out[0]];
            }
            const auto kernel = combinations >> (2 * t * rank);
            for (unsigned delta_tag = 0; delta_tag < histogram.size(); ++delta_tag) {
                check(histogram[delta_tag] == (combinations - kernel) / 4 + (delta_tag == 0 ? kernel : 0),
                      "exhaustive rank distribution differs from Eq. (14)");
            }
            std::cout << "rank=" << rank << ", t=" << t << ", counts=";
            for (const unsigned count : histogram) std::cout << ' ' << count;
            std::cout << '\n';
        }
    }
}

void protocol_test() {
    std::mt19937_64 rng(179);
    const Params p{179, 4, 61, 2};
    const Key key{42};
    Metadata meta{p, Id{1}, Id{2}, 12 * p.block_bytes() - 1, 12, 8};
    const auto shared = std::make_shared<const Authenticator>(key, meta);
    const Owner owner(key, meta, Method::unified, shared);
    const Owner column(key, meta, Method::column_tail);
    const Owner on_the_fly(key, meta, Method::on_the_fly);
    auto version = meta;
    version.file = Id{3};
    const Owner next(key, version, Method::unified, shared);
    const Owner next_derived(key, version);
    const auto block = random_block(p, rng);
    const auto h = owner.evaluate(block);
    check(h == column.evaluate(block) && h == on_the_fly.evaluate(block), "FCA variants diverged");
    check(h == next.evaluate(block) && h == next_derived.evaluate(block), "scope reuse failed");
    check(owner.mask(0) != next.mask(0) && owner.mask(0) != owner.mask(1), "file/block binding failed");
    auto other_scope = version;
    other_scope.scope = Id{4};
    rejects([&] { Owner(key, other_scope, Method::unified, shared); });
    rejects([&] { Owner(Key{43}, version, Method::unified, shared); });
    auto standalone = meta;
    standalone.scope.reset();
    rejects([&] { Owner(key, standalone, Method::unified, shared); });
    standalone.file = Id{5};
    const Owner standalone_one(key, standalone);
    standalone.file = Id{6};
    const Owner standalone_two(key, standalone);
    check(standalone_one.evaluate(block) != standalone_two.evaluate(block),
          "standalone files should derive separate factors");

    const Challenge challenge{meta.file, 77, Key{5}};
    auto request = serialize_challenge(challenge);
    check(request.size() == 56 && parse_challenge(request) == challenge, "challenge wire round trip");
    request.push_back(0);
    rejects([&] { parse_challenge(request); });
    check(parse_metadata(serialize_metadata(meta, true), true) == meta, "private metadata round trip");
    auto public_meta = parse_metadata(serialize_metadata(meta, false), false);
    check(!public_meta.scope && public_meta.file == meta.file, "public metadata leaked scope");
    const auto terms = expand(meta, challenge);
    check(terms == expand(public_meta, challenge), "seed expansion differs between roles");
    check(terms.size() == meta.samples, "wrong sample size");
    const Algebra algebra(p);
    std::set<std::uint32_t> indices;
    Proof proof{meta.file, challenge.round, Block(p.block_words()), {}};
    for (const auto& term : terms) {
        check(term.index < meta.count && indices.insert(term.index).second, "invalid sampled index");
        check(term.coefficient != Field{} && algebra.canonical(term.coefficient), "invalid coefficient");
        const auto portion = random_block(p, rng);
        algebra.add_scaled(proof.aggregate, portion, term.coefficient);
        proof.tag ^= algebra.multiply(owner.tag(term.index, portion), term.coefficient);
    }
    check(owner.verify(challenge, proof), "honest audit failed");
    auto wire = serialize_proof(p, proof);
    check(wire.size() == 24 + p.block_bytes() + p.field_bytes(), "wrong proof wire length");
    check(parse_proof(p, wire) == proof, "proof wire round trip");
    wire.back() |= 0x80;
    rejects([&] { parse_proof(p, wire); });
    wire = serialize_proof(p, proof);
    wire[24 + p.word_bytes() - 1] |= 0x80;
    rejects([&] { parse_proof(p, wire); });
    wire.pop_back();
    rejects([&] { parse_proof(p, wire); });
    auto changed = proof;
    changed.aggregate[0] ^= 1;
    check(!owner.verify(challenge, changed), "corrupted data accepted");
    changed = proof;
    changed.tag[0] ^= 1;
    check(!owner.verify(challenge, changed), "corrupted tag accepted");
    changed = proof;
    changed.round++;
    check(!owner.verify(challenge, changed), "wrong round accepted");
    changed = proof;
    changed.file[0] ^= 1;
    check(!owner.verify(challenge, changed), "wrong FID accepted");
}

}  // namespace

int main() {
    try {
        algebra_test();
        rank_distribution_test();
        protocol_test();
        std::cout << "algebra, rank distribution, scope, wire, and auditing tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
