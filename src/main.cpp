#include "local.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {

using namespace fca;

template <typename Integer>
Integer number(std::string_view value) {
    Integer result;
    const auto end = value.data() + value.size();
    auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) throw std::invalid_argument("invalid integer");
    return result;
}

void usage() {
    std::cerr <<
        "fca keygen KEY\n"
        "fca scope\n"
        "fca prepare KEY INPUT STORE OWNER_META [--scope HEX] [--samples C]\n"
        "            [--branches T] [--prime P] [--columns L] [--width W]\n"
        "fca challenge KEY OWNER_META ROUND REQUEST\n"
        "fca prove STORE REQUEST RESPONSE\n"
        "fca verify KEY OWNER_META REQUEST RESPONSE\n";
}

int run(int argc, char* argv[]) {
    const std::string_view operation = argc > 1 ? argv[1] : "";
    if (operation == "keygen" && argc == 3) {
        write_key(argv[2], random_key());
        return 0;
    }
    if (operation == "scope" && argc == 2) {
        std::cout << hex(random_id()) << '\n';
        return 0;
    }
    if (operation == "prepare" && argc >= 6 && argc % 2 == 0) {
        Metadata meta;
        meta.file = random_id();
        for (int i = 6; i < argc; i += 2) {
            const std::string_view option = argv[i];
            const std::string_view value = argv[i + 1];
            if (option == "--scope") {
                const auto scope = unhex(value);
                if (scope.size() != Id{}.size()) throw std::invalid_argument("scope must be 16 bytes");
                meta.scope.emplace();
                std::ranges::copy(scope, meta.scope->begin());
            } else {
                auto parsed = number<std::uint32_t>(value);
                if (parsed == 0) throw std::invalid_argument("prepare options must be positive");
                if (option == "--samples") meta.samples = parsed;
                else if (option == "--branches") meta.params.branches = parsed;
                else if (option == "--prime") meta.params.p = parsed;
                else if (option == "--columns") meta.params.columns = parsed;
                else if (option == "--width") meta.params.width = parsed;
                else throw std::invalid_argument("unknown prepare option");
            }
        }
        prepare(read_key(argv[2]), argv[3], argv[4], argv[5], meta);
        std::cout << "FID " << hex(meta.file) << '\n';
        return 0;
    }
    if (operation == "challenge" && argc == 6) {
        const auto meta = parse_metadata(read_file(argv[3], 69), true);
        const Owner owner(read_key(argv[2]), meta, Method::on_the_fly);
        write_file(argv[5], serialize_challenge(owner.challenge(number<std::uint64_t>(argv[4]))));
        return 0;
    }
    if (operation == "prove" && argc == 5) {
        const fs::path store = argv[2];
        const auto meta = parse_metadata(read_file(store / "public.bin", 52), false);
        const auto challenge = parse_challenge(read_file(argv[3], 56));
        BlockReader blocks(store / "blocks.bin", meta);
        TagReader tags(store / "tags.bin", meta);
        write_file(argv[4], serialize_proof(meta.params, prove(meta, challenge, blocks, tags)));
        return 0;
    }
    if (operation == "verify" && argc == 6) {
        const auto meta = parse_metadata(read_file(argv[3], 69), true);
        const Owner owner(read_key(argv[2]), meta);
        bool accepted = false;
        try {
            const auto challenge = parse_challenge(read_file(argv[4], 56));
            const auto response_size = 24 + meta.params.block_bytes() + meta.params.field_bytes();
            const auto proof = parse_proof(meta.params, read_file(argv[5], response_size));
            accepted = owner.verify(challenge, proof);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
        std::cout << (accepted ? "ACCEPT\n" : "REJECT\n");
        return accepted ? 0 : 1;
    }
    usage();
    return 2;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
