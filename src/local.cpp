#include "local.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace fca {
namespace {

std::ifstream open_input(const fs::path& path) {
    if (!fs::is_regular_file(path)) throw std::runtime_error("expected regular file: " + path.string());
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + path.string());
    return stream;
}

std::ofstream open_output(const fs::path& path) {
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    return stream;
}

void read_record(std::ifstream& stream, std::uint32_t index, std::uint32_t count,
                 std::span<std::uint8_t> record) {
    if (index >= count) throw std::out_of_range("block index");
    const std::uint64_t offset = std::uint64_t(index) * record.size();
    if (offset > std::numeric_limits<std::streamoff>::max()) {
        throw std::runtime_error("storage offset exceeds stream limits");
    }
    stream.seekg(static_cast<std::streamoff>(offset));
    if (!stream.read(reinterpret_cast<char*>(record.data()), record.size())) {
        throw std::runtime_error("cannot read complete storage record");
    }
}

void check_store_size(const fs::path& path, std::uint64_t record, std::uint32_t count) {
    if (record > std::numeric_limits<std::uint64_t>::max() / count ||
        fs::file_size(path) != record * count) {
        throw std::runtime_error("wrong storage file length: " + path.string());
    }
}

}  // namespace

Bytes read_file(const fs::path& path, std::size_t limit) {
    auto stream = open_input(path);
    const auto size = fs::file_size(path);
    if (size > limit) throw std::runtime_error("file exceeds expected length: " + path.string());
    Bytes bytes(size);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
        stream.peek() != std::istream::traits_type::eof()) {
        throw std::runtime_error("file size changed while reading: " + path.string());
    }
    return bytes;
}

void write_bytes(std::ostream& out, std::span<const std::uint8_t> data) {
    if (!out.write(reinterpret_cast<const char*>(data.data()), data.size())) {
        throw std::runtime_error("cannot write file");
    }
}

void write_file(const fs::path& path, std::span<const std::uint8_t> data) {
    auto out = open_output(path);
    write_bytes(out, data);
    out.close();
}

Key read_key(const fs::path& path) {
    const auto bytes = read_file(path, Key{}.size());
    if (bytes.size() != Key{}.size()) throw std::runtime_error("wrong key length");
    Key key;
    std::ranges::copy(bytes, key.begin());
    return key;
}

void write_key(const fs::path& path, const Key& key) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd == -1) throw std::system_error(errno, std::generic_category(), "key file creation");
    std::size_t written = 0;
    while (written < key.size()) {
        const auto added = ::write(fd, key.data() + written, key.size() - written);
        if (added < 0 && errno == EINTR) continue;
        if (added <= 0) {
            const int error = added < 0 ? errno : EIO;
            ::close(fd);
            ::unlink(path.c_str());
            throw std::system_error(error, std::generic_category(), "key file write");
        }
        written += added;
    }
    if (::close(fd) != 0) {
        const int error = errno;
        ::unlink(path.c_str());
        throw std::system_error(error, std::generic_category(), "key file close");
    }
}

BlockReader::BlockReader(const fs::path& file, const Metadata& meta)
    : meta_(meta), stream_(open_input(file)) {
    meta_.validate();
    check_store_size(file, meta_.params.block_bytes(), meta_.count);
    buffer_.resize(meta_.params.block_bytes());
}

Block BlockReader::read(std::uint32_t index) {
    read_record(stream_, index, meta_.count, buffer_);
    return parse_block(meta_.params, buffer_);
}

TagReader::TagReader(const fs::path& file, const Metadata& meta)
    : meta_(meta), stream_(open_input(file)) {
    meta_.validate();
    check_store_size(file, meta_.params.field_bytes(), meta_.count);
}

Field TagReader::read(std::uint32_t index) {
    auto wire = std::span(buffer_).first(meta_.params.field_bytes());
    read_record(stream_, index, meta_.count, wire);
    return parse_field(meta_.params, wire);
}

void prepare(const Key& key, const fs::path& input, const fs::path& store,
             const fs::path& owner_metadata, Metadata meta) {
    meta.params.validate();
    if (meta.params.width % 8 != 0) throw std::invalid_argument("file width must be divisible by 8");

    auto source = open_input(input);
    meta.size = fs::file_size(input);
    const auto bytes = meta.params.block_bytes();
    const auto count = std::max(std::uint64_t{1}, meta.size / bytes + (meta.size % bytes != 0));
    if (count > UINT32_MAX) throw std::invalid_argument("file has too many blocks");
    meta.count = count;
    if (meta.samples == 0) meta.samples = std::min(meta.count, std::uint32_t{688});
    const Owner owner(key, meta);

    if (fs::exists(owner_metadata)) throw std::runtime_error("owner metadata already exists");
    if (!fs::create_directory(store)) throw std::runtime_error("store directory already exists");
    auto staging = owner_metadata;
    staging += "." + hex(meta.file) + ".tmp";
    try {
        auto blocks = open_output(store / "blocks.bin");
        auto tags = open_output(store / "tags.bin");
        Bytes raw(bytes);
        auto remaining = meta.size;
        for (std::uint32_t i = 0; i < meta.count; ++i) {
            const auto size = std::min<std::uint64_t>(bytes, remaining);
            if (!source.read(reinterpret_cast<char*>(raw.data()), size)) {
                throw std::runtime_error("source ended before expected size");
            }
            std::fill(raw.begin() + size, raw.end(), 0);
            remaining -= size;
            write_bytes(blocks, raw);
            const auto tag = owner.tag(i, parse_block(meta.params, raw));
            write_bytes(tags, serialize_field(meta.params, tag));
        }
        if (source.peek() != std::istream::traits_type::eof()) {
            throw std::runtime_error("source grew while preparing");
        }
        blocks.close();
        tags.close();
        write_file(store / "public.bin", serialize_metadata(meta, false));
        write_file(staging, serialize_metadata(meta, true));
        // Hard linking the staged file publishes it atomically without overwriting another owner.
        fs::create_hard_link(staging, owner_metadata);
    } catch (...) {
        std::error_code ignored;
        fs::remove(staging, ignored);
        fs::remove_all(store, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove(staging, ignored);
}

Proof prove(const Metadata& meta, const Challenge& challenge, BlockReader& blocks, TagReader& tags) {
    const Algebra algebra(meta.params);
    Proof proof{meta.file, challenge.round, Block(meta.params.block_words()), {}};
    for (const auto& term : expand(meta, challenge)) {
        algebra.add_scaled(proof.aggregate, blocks.read(term.index), term.coefficient);
        proof.tag ^= algebra.multiply(tags.read(term.index), term.coefficient);
    }
    return proof;
}

}  // namespace fca
