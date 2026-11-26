#include "fca.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace fca {

Field evaluate_factors(const Algebra&, const Block&, const std::vector<Factors>&, bool);

namespace {

constexpr std::string_view protocol = "FCA-local-v1";
constexpr std::uint8_t magic[] = {'F', 'C', 'A', 1};

void put(Bytes& out, std::uint64_t value, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i) out.push_back((value >> (8 * i)) & 0xff);
}

void append(Bytes& out, std::span<const std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_string(Bytes& out, std::string_view value) {
    put(out, value.size(), 1);
    out.insert(out.end(), value.begin(), value.end());
}

void put_params(Bytes& out, const Params& params) {
    put(out, params.p);
    put(out, params.columns);
    put(out, params.width);
    put(out, params.branches);
}

struct Reader {
    std::span<const std::uint8_t> input;

    std::span<const std::uint8_t> take(std::size_t count) {
        if (input.size() < count) throw std::invalid_argument("truncated encoding");
        const auto value = input.first(count);
        input = input.subspan(count);
        return value;
    }

    std::uint64_t integer(unsigned size = 4) {
        const auto bytes = take(size);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(bytes[i]) << (8 * i);
        return value;
    }

    template <std::size_t N>
    std::array<std::uint8_t, N> array() {
        const auto bytes = take(N);
        std::array<std::uint8_t, N> value;
        std::ranges::copy(bytes, value.begin());
        return value;
    }

    void finish() const {
        if (!input.empty()) throw std::invalid_argument("trailing data");
    }
};

Key prf(const Key& key, std::span<const std::uint8_t> msg) {
    Key result;
    unsigned length = 0;
    if (!HMAC(EVP_sha256(), key.data(), key.size(), msg.data(), msg.size(),
              result.data(), &length) || length != result.size()) {
        throw std::runtime_error("HMAC-SHA256 failed");
    }
    return result;
}

Bytes domain(std::string_view label) {
    Bytes msg;
    msg.reserve(80);
    put_string(msg, protocol);
    put_string(msg, label);
    return msg;
}

Key derive_root(const Key& master, std::string_view label, const Id& id) {
    auto msg = domain(label);
    append(msg, id);
    return prf(master, msg);
}

Bytes make_factor_context(const Metadata& meta) {
    auto msg = domain("factor-context");
    put_params(msg, meta.params);
    put(msg, meta.scope.has_value(), 1);
    append(msg, meta.scope.value_or(meta.file));
    return msg;
}

Bytes make_mask_context(const Metadata& meta) {
    auto msg = domain("block-mask");
    put_params(msg, meta.params);
    append(msg, meta.file);
    return msg;
}

Field unpack_field(std::span<const std::uint8_t> bytes) {
    Field value{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
        value[i / 8] |= Word(bytes[i]) << (8 * (i % 8));
    }
    return value;
}

Field digest_field(const Params& params, Key digest) {
    const unsigned n = params.degree();
    if (n % 8 != 0) digest[n / 8] &= (1U << (n % 8)) - 1;
    return unpack_field(std::span(digest).first(params.field_bytes()));
}

Field derive_field(const Params& params, const Key& root, const Bytes& context,
                   std::string_view label, std::uint32_t branch, std::uint32_t index) {
    auto msg = context;
    put_string(msg, label);
    put(msg, branch);
    put(msg, index);
    return digest_field(params, prf(root, msg));
}

template <std::size_t N>
std::array<std::uint8_t, N> random_bytes() {
    std::array<std::uint8_t, N> output;
    if (RAND_bytes(output.data(), N) != 1) throw std::runtime_error("RAND_bytes failed");
    return output;
}

}  // namespace

void Metadata::validate() const {
    params.validate();
    const auto bytes = params.block_bytes();
    const auto expected = std::max(std::uint64_t{1}, size / bytes + (size % bytes != 0));
    if (count != expected || samples == 0 || samples > count) {
        throw std::invalid_argument("invalid size, block count, or challenge size");
    }
}

Key random_key() { return random_bytes<32>(); }
Id random_id() { return random_bytes<16>(); }

Bytes unhex(std::string_view input) {
    if (input.size() % 2 != 0) throw std::invalid_argument("invalid hex length");
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("invalid hex digit");
    };
    Bytes bytes;
    for (unsigned i = 0; i < input.size(); i += 2) {
        bytes.push_back((digit(input[i]) << 4) | digit(input[i + 1]));
    }
    return bytes;
}

std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr auto digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

Authenticator::Authenticator(const Key& master, Metadata meta, Method method)
    : algebra_(meta.params), method_(method),
      root_(derive_root(master, meta.scope ? "scope" : "standalone", meta.scope.value_or(meta.file))),
      context_(make_factor_context(meta)) {
    const auto& params = algebra_.params();
    if (method_ == Method::dense) {
        dense_.resize(std::size_t(params.columns) * params.width);
        for (unsigned j = 0; j < params.columns; ++j) {
            for (unsigned k = 0; k < params.width; ++k) {
                dense_[std::size_t(j) * params.width + k] =
                    derive_field(params, root_, context_, "dense", j, k);
            }
        }
    } else if (method_ == Method::polynomial) {
        alpha_ = derive_field(params, root_, context_, "polynomial", 0, 0);
    } else if (method_ != Method::on_the_fly) {
        factors_ = derive_factors();
    }
}

Authenticator::Authenticator(const Metadata& meta, std::vector<Factors> factors)
    : algebra_(meta.params), method_(Method::unified), factors_(std::move(factors)) {
    if (factors_.size() != meta.params.branches) {
        throw std::invalid_argument("invalid factor branch count");
    }
    for (const auto& branch : factors_) {
        if (branch.columns.size() != meta.params.columns || branch.planes.size() != meta.params.width ||
            !std::ranges::all_of(branch.columns, [this](const Field& v) { return algebra_.canonical(v); }) ||
            !std::ranges::all_of(branch.planes, [this](const Field& v) { return algebra_.canonical(v); })) {
            throw std::invalid_argument("invalid factors");
        }
    }
}

std::vector<Factors> Authenticator::derive_factors() const {
    const auto& params = algebra_.params();
    std::vector<Factors> result(params.branches);
    for (unsigned s = 0; s < params.branches; ++s) {
        auto& branch = result[s];
        branch.columns.resize(params.columns);
        branch.planes.resize(params.width);
        for (unsigned j = 0; j < params.columns; ++j) {
            branch.columns[j] = derive_field(params, root_, context_, "factor-a", s, j);
        }
        for (unsigned k = 0; k < params.width; ++k) {
            branch.planes[k] = derive_field(params, root_, context_, "factor-b", s, k);
        }
    }
    return result;
}

std::size_t Authenticator::elements() const {
    const auto& p = algebra_.params();
    if (method_ == Method::dense) return dense_.size();
    if (method_ == Method::polynomial) return 1;
    if (method_ == Method::on_the_fly) return 0;
    return std::size_t(p.branches) * (std::size_t(p.columns) + p.width);
}

Field Authenticator::evaluate(const Block& block) const {
    if (!algebra_.canonical(block)) throw std::invalid_argument("noncanonical block");
    if (method_ == Method::dense) {
        const auto values = algebra_.encode(block);
        Field h{};
        for (std::size_t i = 0; i < values.size(); ++i) h ^= algebra_.multiply(values[i], dense_[i]);
        return h;
    }
    if (method_ == Method::polynomial) {
        const auto values = algebra_.encode(block);
        Field h{};
        for (auto i = values.rbegin(); i != values.rend(); ++i) {
            h = algebra_.multiply(h ^ *i, alpha_);
        }
        return h;
    }
    if (method_ == Method::on_the_fly) {
        return evaluate_factors(algebra_, block, derive_factors(), false);
    }
    return evaluate_factors(algebra_, block, factors_, method_ == Method::column_tail);
}

Owner::Owner(const Key& master, Metadata meta, Method method,
             std::shared_ptr<const Authenticator> shared)
    : meta_(meta), algebra_(meta.params), auth_(std::move(shared)),
      mask_root_(derive_root(master, "file", meta.file)), mask_context_(make_mask_context(meta)) {
    meta_.validate();
    if (!auth_) {
        auth_ = std::make_shared<const Authenticator>(master, meta, method);
    } else if (auth_->method_ != method || auth_->context_ != make_factor_context(meta) ||
               auth_->root_ != derive_root(master, meta.scope ? "scope" : "standalone",
                                          meta.scope.value_or(meta.file))) {
        throw std::invalid_argument("authenticator does not match owner context");
    }
}

Field Owner::mask(std::uint32_t index) const {
    if (index >= meta_.count) throw std::out_of_range("block index");
    return derive_field(meta_.params, mask_root_, mask_context_, "rho", 0, index);
}

Field Owner::aggregate_mask(std::span<const Term> terms) const {
    Field r{};
    for (const auto& term : terms) r ^= algebra_.multiply(term.coefficient, mask(term.index));
    return r;
}

Field Owner::tag(std::uint32_t index, const Block& block) const {
    return mask(index) ^ evaluate(block);
}

Challenge Owner::challenge(std::uint64_t round) const {
    return {meta_.file, round, random_key()};
}

bool Owner::verify(const Challenge& challenge, const Proof& proof) const {
    if (challenge.file != meta_.file || proof.file != challenge.file ||
        proof.round != challenge.round || !algebra_.canonical(proof.aggregate) ||
        !algebra_.canonical(proof.tag)) return false;

    const auto expected = evaluate(proof.aggregate) ^ aggregate_mask(expand(meta_, challenge));
    return CRYPTO_memcmp(expected.data(), proof.tag.data(), sizeof(Field)) == 0;
}

std::vector<Term> expand(const Metadata& meta, const Challenge& challenge) {
    meta.validate();
    if (meta.file != challenge.file) throw std::invalid_argument("challenge FID mismatch");

    auto context = domain("challenge");
    append(context, challenge.file);
    put(context, challenge.round, 8);
    put(context, meta.params.p);
    put(context, meta.params.columns);
    put(context, meta.params.width);
    put(context, meta.count);
    put(context, meta.samples);
    const Key index_key = derive_root(challenge.seed, "index-stream", meta.file);
    const Key coefficient_key = derive_root(challenge.seed, "coeff-stream", meta.file);

    std::uint64_t index_draw = 0;
    std::uint64_t coefficient_draw = 0;
    auto draw = [&context](const Key& key, std::uint64_t& counter) {
        auto message = context;
        put(message, counter++, 8);
        return prf(key, message);
    };

    // Partial Fisher-Yates, storing only changed positions.
    std::vector<Term> terms;
    terms.reserve(meta.samples);
    std::unordered_map<std::uint32_t, std::uint32_t> swapped;
    swapped.reserve(meta.samples);
    auto value_at = [&swapped](std::uint32_t position) {
        const auto found = swapped.find(position);
        return found == swapped.end() ? position : found->second;
    };
    for (std::uint32_t i = 0; i < meta.samples; ++i) {
        const std::uint64_t range = meta.count - i;
        const std::uint64_t threshold = (std::uint64_t{0} - range) % range;
        std::uint64_t sample;
        do {
            const auto digest = draw(index_key, index_draw);
            Reader reader{digest};
            sample = reader.integer(8);
        } while (sample < threshold);
        const std::uint32_t position = i + sample % range;
        const std::uint32_t index = value_at(position);
        if (position != i) swapped[position] = value_at(i);
        swapped.erase(i);
        Field coefficient;
        do {
            coefficient = digest_field(meta.params, draw(coefficient_key, coefficient_draw));
        } while (coefficient == Field{});
        terms.push_back({index, coefficient});
    }
    return terms;
}

Bytes serialize_metadata(const Metadata& meta, bool include_scope) {
    meta.validate();
    Bytes wire;
    append(wire, magic);
    put_params(wire, meta.params);
    append(wire, meta.file);
    put(wire, meta.size, 8);
    put(wire, meta.count);
    put(wire, meta.samples);
    if (include_scope) {
        put(wire, meta.scope.has_value(), 1);
        if (meta.scope) append(wire, *meta.scope);
    }
    return wire;
}

Metadata parse_metadata(std::span<const std::uint8_t> wire, bool include_scope) {
    Reader reader{wire};
    if (!std::ranges::equal(reader.take(sizeof(magic)), magic)) {
        throw std::invalid_argument("unrecognized metadata");
    }
    Metadata meta;
    auto& params = meta.params;
    params.p = reader.integer();
    params.columns = reader.integer();
    params.width = reader.integer();
    params.branches = reader.integer();
    meta.file = reader.array<16>();
    meta.size = reader.integer(8);
    meta.count = reader.integer();
    meta.samples = reader.integer();
    if (include_scope) {
        const auto scoped = reader.integer(1);
        if (scoped > 1) throw std::invalid_argument("invalid scope flag");
        if (scoped) meta.scope = reader.array<16>();
    }
    reader.finish();
    meta.validate();
    return meta;
}

Bytes serialize_challenge(const Challenge& challenge) {
    Bytes wire;
    append(wire, challenge.file);
    put(wire, challenge.round, 8);
    append(wire, challenge.seed);
    return wire;
}

Challenge parse_challenge(std::span<const std::uint8_t> wire) {
    Reader reader{wire};
    Challenge challenge{reader.array<16>(), reader.integer(8), reader.array<32>()};
    reader.finish();
    return challenge;
}

Bytes serialize_field(const Params& params, const Field& value) {
    if (!Algebra(params).canonical(value)) throw std::invalid_argument("noncanonical field");
    Bytes wire;
    const unsigned size = params.field_bytes();
    wire.reserve(size);
    for (unsigned i = 0; i < size; ++i) wire.push_back(value[i / 8] >> (8 * (i % 8)));
    return wire;
}

Field parse_field(const Params& params, std::span<const std::uint8_t> wire) {
    const Algebra algebra(params);
    if (wire.size() != params.field_bytes()) throw std::invalid_argument("invalid field length");
    const auto value = unpack_field(wire);
    if (!algebra.canonical(value)) throw std::invalid_argument("noncanonical field");
    return value;
}

Bytes serialize_block(const Params& params, const Block& block) {
    if (!Algebra(params).canonical(block)) throw std::invalid_argument("noncanonical block");
    Bytes wire;
    wire.reserve(params.block_bytes());
    for (const auto word : block) put(wire, word, params.word_bytes());
    return wire;
}

Block parse_block(const Params& params, std::span<const std::uint8_t> wire) {
    const Algebra algebra(params);
    if (wire.size() != params.block_bytes()) throw std::invalid_argument("invalid block length");
    Block block(params.block_words());
    const auto word_bytes = params.word_bytes();
    if (std::endian::native == std::endian::little && word_bytes == sizeof(Word)) {
        std::memcpy(block.data(), wire.data(), wire.size());
    } else {
        Reader reader{wire};
        for (auto& word : block) word = reader.integer(word_bytes);
    }
    if (!algebra.canonical(block)) throw std::invalid_argument("noncanonical block");
    return block;
}

Bytes serialize_proof(const Params& params, const Proof& proof) {
    Bytes wire;
    wire.reserve(24 + params.block_bytes() + params.field_bytes());
    append(wire, proof.file);
    put(wire, proof.round, 8);
    append(wire, serialize_block(params, proof.aggregate));
    append(wire, serialize_field(params, proof.tag));
    return wire;
}

Proof parse_proof(const Params& params, std::span<const std::uint8_t> wire) {
    params.validate();
    Reader reader{wire};
    Proof proof{reader.array<16>(), reader.integer(8),
                parse_block(params, reader.take(params.block_bytes())),
                parse_field(params, reader.take(params.field_bytes()))};
    reader.finish();
    return proof;
}

}  // namespace fca
