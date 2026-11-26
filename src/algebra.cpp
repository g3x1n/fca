#include "fca.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

#if !defined(FCA_PORTABLE) && defined(__aarch64__) && defined(__ARM_FEATURE_AES)
#include <arm_neon.h>
#define FCA_PMULL
#elif !defined(FCA_PORTABLE) && defined(__PCLMUL__)
#include <wmmintrin.h>
#endif

namespace fca {
namespace {

bool prime(unsigned p) {
    if (p < 2) return false;
    for (unsigned i = 2; i <= p / i; ++i) {
        if (p % i == 0) return false;
    }
    return true;
}

Word low_bits(unsigned count) {
    return count == 64 ? ~Word{0} : (Word{1} << count) - 1;
}

std::array<Word, 2> carryless(Word a, Word b) {
#if defined(FCA_PMULL)
    auto product = vreinterpretq_u64_p128(vmull_p64(a, b));
    return {vgetq_lane_u64(product, 0), vgetq_lane_u64(product, 1)};
#elif !defined(FCA_PORTABLE) && defined(__PCLMUL__)
    auto product = _mm_clmulepi64_si128(_mm_set_epi64x(0, a), _mm_set_epi64x(0, b), 0);
    return {static_cast<Word>(_mm_cvtsi128_si64(product)),
            static_cast<Word>(_mm_cvtsi128_si64(_mm_srli_si128(product, 8)))};
#else
    std::array<Word, 2> product{};
    for (unsigned i = 0; i < 64; ++i) {
        Word selected = Word{0} - ((b >> i) & 1);
        product[0] ^= (a << i) & selected;
        if (i != 0) product[1] ^= (a >> (64 - i)) & selected;
    }
    return product;
#endif
}

void xor_words(Word* target, const Word* source, unsigned count) {
    for (unsigned i = 0; i < count; ++i) target[i] ^= source[i];
}

void fold(Word* target, const Word* source, unsigned degree) {
    const Word tail = source[degree];
    for (unsigned i = 0; i < degree; ++i) target[i] ^= source[i] ^ tail;
}

// Add a length-p circular product. Input's implicit coefficient of X^(p-1) is zero.
void circular(Word* target, const Word* column, unsigned p, const Field& scalar) {
    const unsigned n = p - 1;
    for (unsigned limb = 0; limb < (n + 63) / 64; ++limb) {
        Word bits = scalar[limb];
        while (bits != 0) {
            const unsigned shift = 64 * limb + std::countr_zero(bits);
            const unsigned first = std::min(p - shift, n);
            xor_words(target + shift, column, first);
            xor_words(target, column + first, n - first);
            bits &= bits - 1;
        }
    }
}

void transpose64(std::array<Word, 64>& rows) {
    // Swap off-diagonal quadrants, halving the tile size at each step.
    Word mask = 0x00000000ffffffffULL;
    for (unsigned shift = 32; shift != 0;) {
        for (unsigned row = 0; row < 64; row = (row + shift + 1) & ~shift) {
            const Word swap = ((rows[row] >> shift) ^ rows[row + shift]) & mask;
            rows[row] ^= swap << shift;
            rows[row + shift] ^= swap;
        }
        shift /= 2;
        mask ^= mask << shift;
    }
}

void planes(const Word* column, unsigned n, std::span<Field> output) {
    for (unsigned offset = 0; offset < n; offset += 64) {
        std::array<Word, 64> tile{};
        for (unsigned r = 0; r < std::min(64U, n - offset); ++r) {
            tile[r] = column[offset + r];
        }
        transpose64(tile);
        for (unsigned k = 0; k < output.size(); ++k) {
            output[k][offset / 64] = tile[k];
        }
    }
}

}  // namespace

void Params::validate() const {
    if (p < 3 || p > 251 || p % 2 == 0 || !prime(p)) {
        throw std::invalid_argument("p must be an odd prime at most 251");
    }
    unsigned power = 2;
    unsigned order = 1;
    while (power != 1) {
        power = (power * 2) % p;
        ++order;
    }
    if (order != degree()) throw std::invalid_argument("2 must have order p-1 modulo p");
    if (columns == 0 || width == 0 || width > 64 || branches == 0) {
        throw std::invalid_argument("columns and branches must be positive, width must be 1..64");
    }
}

Algebra::Algebra(Params params) : params_(params) { params_.validate(); }

bool Algebra::canonical(const Field& value) const {
    for (unsigned i = 0; i < value.size(); ++i) {
        const int remaining = int(params_.degree()) - int(64 * i);
        const auto bits = unsigned(std::clamp(remaining, 0, 64));
        if ((value[i] & ~low_bits(bits)) != 0) return false;
    }
    return true;
}

bool Algebra::canonical(const Block& block) const {
    if (block.size() != params_.block_words()) return false;
    const Word forbidden = ~low_bits(params_.width);
    return forbidden == 0 ||
           std::none_of(block.begin(), block.end(), [forbidden](Word word) {
               return (word & forbidden) != 0;
           });
}

Field Algebra::multiply(const Field& a, const Field& b) const {
    const unsigned n = params_.degree();
    const unsigned limbs = (n + 63) / 64;
    std::array<Word, 8> product{};
    for (unsigned i = 0; i < limbs; ++i) {
        for (unsigned j = 0; j < limbs; ++j) {
            const auto [lo, hi] = carryless(a[i], b[j]);
            product[i + j] ^= lo;
            product[i + j + 1] ^= hi;
        }
    }
    // The product degree is below 2p, so one wrap modulo X^p+1 suffices.
    const unsigned shift = params_.p % 64;
    const unsigned base = params_.p / 64;
    Field result{};
    for (unsigned i = 0; i < limbs; ++i) {
        result[i] = product[i] ^ (product[i + base] >> shift) ^
                    (product[i + base + 1] << (64 - shift));
    }
    const unsigned tail_shift = n % 64;
    const Word tail = Word{0} - ((product[n / 64] >> tail_shift) & 1);
    for (unsigned i = 0; i < limbs; ++i) result[i] ^= tail;
    result[limbs - 1] &= low_bits(tail_shift == 0 ? 64 : tail_shift);
    return result;
}

std::vector<Field> Algebra::encode(const Block& block) const {
    if (!canonical(block)) throw std::invalid_argument("noncanonical block");
    std::vector<Field> output(std::size_t(params_.columns) * params_.width);
    const unsigned n = params_.degree();
    for (unsigned j = 0; j < params_.columns; ++j) {
        planes(block.data() + std::size_t(j) * n, n,
               std::span(output).subspan(std::size_t(j) * params_.width, params_.width));
    }
    return output;
}

void Algebra::add_scaled(Block& target, const Block& block, const Field& scalar) const {
    if (!canonical(target) || !canonical(block) || !canonical(scalar)) {
        throw std::invalid_argument("invalid scaled block input");
    }
    const unsigned n = params_.degree();
    std::array<Word, 251> product;
    for (unsigned j = 0; j < params_.columns; ++j) {
        const auto offset = std::size_t(j) * n;
        std::fill_n(product.data(), params_.p, Word{0});
        circular(product.data(), block.data() + offset, params_.p, scalar);
        fold(target.data() + offset, product.data(), n);
    }
}

Field evaluate_factors(const Algebra& algebra, const Block& block,
                       const std::vector<Factors>& factors, bool column_tail) {
    const auto& params = algebra.params();
    const unsigned n = params.degree();
    Field h{};
    for (const auto& branch : factors) {
        std::array<Word, 251> total{};
        std::array<Word, 251> product;
        for (unsigned j = 0; j < params.columns; ++j) {
            const auto* column = block.data() + std::size_t(j) * n;
            if (column_tail) {
                std::fill_n(product.data(), params.p, Word{0});
                circular(product.data(), column, params.p, branch.columns[j]);
                fold(total.data(), product.data(), n);
            } else {
                circular(total.data(), column, params.p, branch.columns[j]);
            }
        }
        if (!column_tail) {
            const Word tail = total[n];
            for (unsigned r = 0; r < n; ++r) total[r] ^= tail;
        }
        std::array<Field, 64> bit_planes{};
        planes(total.data(), n, std::span(bit_planes).first(params.width));
        for (unsigned k = 0; k < params.width; ++k) {
            const auto term = algebra.multiply(bit_planes[k], branch.planes[k]);
            for (unsigned i = 0; i < h.size(); ++i) h[i] ^= term[i];
        }
    }
    return h;
}

}  // namespace fca
