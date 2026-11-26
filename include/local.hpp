#pragma once

#include "fca.hpp"

#include <filesystem>
#include <fstream>
#include <functional>

namespace fca {

namespace fs = std::filesystem;

Bytes read_file(const fs::path& path, std::size_t limit);
void write_file(const fs::path& path, std::span<const std::uint8_t> data);
Key read_key(const fs::path& path);
void write_key(const fs::path& path, const Key& key);
void write_bytes(std::ostream& out, std::span<const std::uint8_t> data);

class BlockReader {
public:
    BlockReader(const fs::path& file, const Metadata& meta);
    Block read(std::uint32_t index);

private:
    Metadata meta_;
    std::ifstream stream_;
    Bytes buffer_;
};

class TagReader {
public:
    TagReader(const fs::path& file, const Metadata& meta);
    Field read(std::uint32_t index);

private:
    Metadata meta_;
    std::ifstream stream_;
    std::array<std::uint8_t, 32> buffer_{};
};

void prepare(const Key& key, const fs::path& input, const fs::path& store,
             const fs::path& owner_metadata, Metadata meta);
Proof prove(const Metadata& meta, const Challenge& challenge,
            BlockReader& blocks, TagReader& tags);

}  // namespace fca
