#!/usr/bin/env python3
"""Restore errors for warning groups downgraded by this Clang driver."""
import re
import shlex
import subprocess
import sys


def strict_flags(output):
    commands=[]
    for line in output.splitlines():
        if not line.lstrip().startswith('"'):
            continue
        command=shlex.split(line)
        if '-cc1' in command:
            commands.append(command)
    if len(commands)!=1:
        raise ValueError('expected exactly one Clang cc1 invocation')
    groups=set()
    for token in commands[0]:
        if token.startswith('-Wno-error='):
            group=token.removeprefix('-Wno-error=')
            if not re.fullmatch('[a-z][a-z0-9-]*',group):
                raise ValueError('invalid downgraded warning group')
            groups.add(group)
    # Forward directly to cc1, after the driver's injected -Wno-error options.
    global_error=['-Xclang','-Werror'] if '-Wno-error' in commands[0] else []
    return global_error+[flag for group in sorted(groups) for flag in ('-Xclang','-Werror='+group)]


def main():
    if len(sys.argv)<2:
        raise ValueError('provide the compiler command')
    command=sys.argv[1:]+['-target','x86_64-apple-macos12.0','-mkernel','-###','-x','c++','-c','/dev/null']
    result=subprocess.run(command,capture_output=True,text=True,check=True,timeout=15)
    print(' '.join(strict_flags(result.stderr)))


if __name__=='__main__':
    try:
        main()
    except (OSError,ValueError,subprocess.SubprocessError) as exc:
        raise SystemExit(str(exc))
