#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Link an isolated proof-trace daemon from a completed CMake Makefiles build.

Reads existing compiler/linker commands; never overwrites production binaries.
Requires compile_commands.json. The wrapper records real calls, not timings.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True, help='must not exist')
    args = parser.parse_args()
    build = args.build_dir.resolve(strict=True)
    source = Path(__file__).with_name('yespower-proof-trace.cpp').resolve(strict=True)
    commands = json.loads((build / 'compile_commands.json').read_text())
    row = next(r for r in commands if Path(r['file']).as_posix().endswith('/src/primitives/block.cpp'))
    compile_command = row.get('arguments') or shlex.split(row['command'])
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    wrapper = work / 'yespower-proof-trace.o'
    compile_command[compile_command.index('-o') + 1] = str(wrapper)
    compile_command[compile_command.index('-c') + 1] = str(source)
    link = shlex.split((build / 'src/CMakeFiles/bitcoind.dir/link.txt').read_text())
    binary = work / 'sugarchaind-proof-trace'
    link[link.index('-o') + 1] = str(binary)
    link.insert(1, '-Wl,--wrap=yespower')
    link.insert(link.index('-o'), str(wrapper))
    metadata = {'compile': compile_command, 'compile_cwd': row['directory'],
                'link': link, 'link_cwd': str(build / 'src'),
                'wrapper_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                'purpose': 'offline proof safety trace; NOT production binary or benchmark'}
    (work / 'commands.json').write_text(json.dumps(metadata, indent=2) + '\n')
    subprocess.run(compile_command, cwd=row['directory'], check=True)
    subprocess.run(link, cwd=build / 'src', check=True)
    metadata['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
    (work / 'commands.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(binary)


if __name__ == '__main__':
    main()
