#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fca {

using Bytes = std::vector<std::uint8_t>;
using Word = std::uint64_t;
using Block = std::vector<Word>;
using Key = std::array<std::uint8_t, 32>;
using Id = std::array<std::uint8_t, 16>;
using Field = std::array<Word, 4>;

inline Field& operator^=(Field& target, const Field& value) {
    for (unsigned i = 0; i < target.size(); ++i) target[i] ^= value[i];
    return target;
}

inline Field operator^(Field left, const Field& right) { return left ^= right; }

struct Params {
    std::uint32_t p = 179;
    std::uint32_t columns = 366;
    std::uint32_t width = 64;
    std::uint32_t branches = 1;
    bool operator==(const Params&) const = default;
    void validate() const;
    unsigned degree() const { return p - 1; }
    unsigned field_bytes() const { return (degree() + 7) / 8; }
    unsigned word_bytes() const { return (width + 7) / 8; }
    std::size_t block_words() const { return std::size_t(columns) * degree(); }
    std::size_t block_bytes() const { return block_words() * word_bytes(); }
};

struct Metadata {
    Params params;
    Id file{};
    std::optional<Id> scope;
    std::uint64_t size = 0;
    std::uint32_t count = 0;
    std::uint32_t samples = 0;
    bool operator==(const Metadata&) const = default;
    void validate() const;
};

struct Challenge {
    Id file;
    std::uint64_t round;
    Key seed;
    bool operator==(const Challenge&) const = default;
};

struct Term {
    std::uint32_t index;  // Zero-based local storage offset.
    Field coefficient;
    bool operator==(const Term&) const = default;
};

struct Proof {
    Id file;
    std::uint64_t round;
    Block aggregate;
    Field tag;
    bool operator==(const Proof&) const = default;
};

enum class Method { unified, column_tail, on_the_fly, dense, polynomial };

class Algebra {
public:
    explicit Algebra(Params params);
    const Params& params() const { return params_; }
    Field multiply(const Field& a, const Field& b) const;
    std::vector<Field> encode(const Block& block) const;
    void add_scaled(Block& target, const Block& block, const Field& scalar) const;
    bool canonical(const Field& value) const;
    bool canonical(const Block& block) const;

private:
    Params params_;
};

struct Factors {
    std::vector<Field> columns;
    std::vector<Field> planes;
};

class Authenticator {
public:
    Authenticator(const Key& master, Metadata meta, Method method = Method::unified);
    Authenticator(const Metadata& meta, std::vector<Factors> factors);
    Field evaluate(const Block& block) const;
    std::size_t elements() const;

private:
    friend class Owner;
    std::vector<Factors> derive_factors() const;

    Algebra algebra_;
    Method method_;
    Key root_{};
    Bytes context_;
    std::vector<Factors> factors_;
    std::vector<Field> dense_;
    Field alpha_{};
};

class Owner {
public:
    Owner(const Key& master, Metadata meta, Method method = Method::unified,
          std::shared_ptr<const Authenticator> shared = {});
    const Metadata& metadata() const { return meta_; }
    Field evaluate(const Block& block) const { return auth_->evaluate(block); }
    Field mask(std::uint32_t index) const;
    Field aggregate_mask(std::span<const Term> terms) const;
    Field tag(std::uint32_t index, const Block& block) const;
    Challenge challenge(std::uint64_t round) const;
    bool verify(const Challenge& challenge, const Proof& proof) const;

private:
    Metadata meta_;
    Algebra algebra_;
    std::shared_ptr<const Authenticator> auth_;
    Key mask_root_{};
    Bytes mask_context_;
};

std::vector<Term> expand(const Metadata& meta, const Challenge& challenge);
Key random_key();
Id random_id();
Bytes unhex(std::string_view hex);
std::string hex(std::span<const std::uint8_t> bytes);

Bytes serialize_metadata(const Metadata& meta, bool include_scope);
Metadata parse_metadata(std::span<const std::uint8_t> wire, bool include_scope);
Bytes serialize_challenge(const Challenge& challenge);
Challenge parse_challenge(std::span<const std::uint8_t> wire);
Bytes serialize_proof(const Params& params, const Proof& proof);
Proof parse_proof(const Params& params, std::span<const std::uint8_t> wire);
Bytes serialize_block(const Params& params, const Block& block);
Block parse_block(const Params& params, std::span<const std::uint8_t> wire);
Bytes serialize_field(const Params& params, const Field& value);
Field parse_field(const Params& params, std::span<const std::uint8_t> wire);

}  // namespace fca
