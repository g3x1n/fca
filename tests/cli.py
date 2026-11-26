import pathlib
import random
import stat
import subprocess
import sys
import tempfile


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="fca-cli-") as tmp:
        cwd = pathlib.Path(tmp)

        def run(*args, code=0):
            out = subprocess.run([binary, *args], cwd=cwd, text=True, capture_output=True)
            assert out.returncode == code, (args, out.returncode, out.stdout, out.stderr)
            return out.stdout.strip()

        def flip(name, offset):
            with (cwd / name).open("r+b") as stream:
                stream.seek(offset)
                old = stream.read(1)
                assert old
                stream.seek(offset)
                stream.write(bytes([old[0] ^ 1]))

        block_bytes = 178 * 366 * 8
        (cwd / "source").write_bytes(random.Random(1).randbytes(block_bytes + 3))
        run("keygen", "key")
        assert stat.S_IMODE((cwd / "key").stat().st_mode) == 0o600
        run("keygen", "key", code=2)
        scope = run("scope")
        run("prepare", "key", "source", "store", "owner", "--scope", scope)
        assert (cwd / "store/blocks.bin").stat().st_size == 2 * block_bytes
        assert (cwd / "store/tags.bin").stat().st_size == 46
        run("prepare", "key", "source", "staging-store", "owner", code=2)
        assert not (cwd / "staging-store").exists()

        run("challenge", "key", "owner", "1", "request")
        run("prove", "store", "request", "proof")
        assert (cwd / "request").stat().st_size == 56
        assert (cwd / "proof").stat().st_size == 24 + block_bytes + 23
        assert run("verify", "key", "owner", "request", "proof") == "ACCEPT"

        run("challenge", "key", "owner", "2", "next-request")
        assert run("verify", "key", "owner", "next-request", "proof", code=1) == "REJECT"
        for name in ("store/blocks.bin", "store/tags.bin"):
            flip(name, 0)  # Both blocks are selected, so record zero is always included.
            run("prove", "store", "request", "damaged-proof")
            assert run("verify", "key", "owner", "request", "damaged-proof", code=1) == "REJECT"
            flip(name, 0)

        (cwd / "proof").write_bytes(b"short")
        assert run("verify", "key", "owner", "request", "proof", code=1) == "REJECT"
        (cwd / "empty").touch()
        run("prepare", "key", "empty", "small", "small-owner", "--prime", "11",
            "--columns", "2", "--width", "8")
        run("challenge", "key", "small-owner", "3", "small-request")
        run("prove", "small", "small-request", "small-proof")
        assert run("verify", "key", "small-owner", "small-request", "small-proof") == "ACCEPT"

    print("local CLI integration passed")


if __name__ == "__main__":
    main()
