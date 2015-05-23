#!/usr/bin/env python3

import sys
import os
from checksetup import *
from commandwrapper import findExe


def irWrapper(var, command):
    wrapper = os.path.join(IR_WRAPPER_DIR, 'bin', command)
    if not os.path.exists(wrapper):
        sys.exit('could not find ' + wrapper)
    return '-DCMAKE_' + var + '=' + wrapper


commandline = [
    findExe('cmake'),
    irWrapper('C_COMPILER', 'clang'),
    irWrapper('CXX_COMPILER', 'clang++'),
    irWrapper('LINKER', 'ld'),
    irWrapper('AR', 'ar'),
    irWrapper('RANLIB', 'ranlib')
]
hasGenerator = any(elem.startswith('-G') for elem in sys.argv)
if not hasGenerator:
    commandline.append('-GNinja')
commandline.extend(sys.argv[1:])  # append all the user passed flags

print("About to run:", quoteCommand(commandline))
os.environ[ENVVAR_NO_EMIT_IR] = '1'
# no need for subprocess.call, just use execvpe
os.execvpe(commandline[0], commandline, os.environ)
sys.exit('Could not execute ' + str(commandline))
