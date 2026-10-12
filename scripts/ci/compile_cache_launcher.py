"""Optional compiler cache; argv is preserved and the real compiler remains authoritative."""
import argparse
from pathlib import Path
import subprocess
import sys


def run(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", required=True)
    parser.add_argument("--cache-argument", action="append", default=[])
    parser.add_argument("--disable-flag", type=Path, required=True)
    options, command = parser.parse_known_args(argv)
    if command and command[0] == "--":
        command.pop(0)
    if not command:
        parser.error("missing real compiler command")
    if not options.disable_flag.exists():
        try:
            cached = subprocess.run([options.cache, *options.cache_argument, *command], check=False)
            if cached.returncode == 0:
                return 0
        except OSError:
            pass
        # A failing cache never replaces the compiler's result. Genuine compiler errors are
        # repeated once and still fail; a cache/storage/service failure recovers through MSVC.
        try:
            options.disable_flag.parent.mkdir(parents=True, exist_ok=True)
            options.disable_flag.touch()
        except OSError:
            pass
        print("Compiler cache unavailable; using the real compiler for this build.", file=sys.stderr)
    try:
        return subprocess.run(command, check=False).returncode
    except OSError as error:
        print("Cannot launch the real compiler: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(run())
