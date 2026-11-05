#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Exercise the Switch install dialog's production copy/lifecycle methods with slow and failing I/O."""
from pathlib import Path
import argparse
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--thread-sanitizer', action='store_true', help='Use TSan instead of ASan/UBSan')
args = parser.parse_args()
source = (root / 'Core/Dialog/PSPGamedataInstallDialog.cpp').read_text()
header = (root / 'Core/Dialog/PSPGamedataInstallDialog.h').read_text()
fixture = (root / 'scripts/tests/SwitchGamedataInstallTest.cpp').read_text()

def function(signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

methods = ['PSPGamedataInstallDialog::~PSPGamedataInstallDialog()',
           'void PSPGamedataInstallDialog::WaitForIO()', 'void PSPGamedataInstallDialog::StopCopy()',
           'void PSPGamedataInstallDialog::FinishCopy(', 'int PSPGamedataInstallDialog::Update(',
           'void PSPGamedataInstallDialog::OpenNextFile()', 'void PSPGamedataInstallDialog::CopyCurrentFileData()',
           'void PSPGamedataInstallDialog::CloseCurrentFile(', 'int PSPGamedataInstallDialog::Abort()',
           'int PSPGamedataInstallDialog::Shutdown(', 'void PSPGamedataInstallDialog::UpdateProgress(']
declaration = header[header.index('struct SceUtilityGamedataInstallParam'):].replace('private:', 'public:')
fixture = fixture.replace('// DIALOG_DECLARATION', declaration).replace('// DIALOG_METHODS', '\n'.join(function(name) for name in methods))
utility = (root / 'Core/HLE/sceUtility.cpp').read_text()
start = utility.index('void __UtilityWaitForIO()')
fixture = fixture.replace('// UTILITY_WAIT', utility[start:utility.index('\nvoid __UtilityShutdown()', start)])
filesystem = (root / 'Core/FileSystems/DirectoryFileSystem.cpp').read_text()
start = filesystem.index('void DirectoryFileHandle::Close()')
fixture = fixture.replace('// FILE_CLOSE', filesystem[start:filesystem.index('\nvoid DirectoryFileSystem::CloseAll()', start)])
assert source[source.index('void PSPGamedataInstallDialog::DoState('):].split('auto s =', 1)[0].count('WaitForIO();') == 1
with tempfile.TemporaryDirectory(prefix='ppsspp-install-test-') as directory:
    cpp, executable = Path(directory) / 'test.cpp', Path(directory) / 'test'
    cpp.write_text(fixture)
    sanitizer = '-fsanitize=thread' if args.thread_sanitizer else '-fsanitize=address,undefined'
    subprocess.run(['clang++', '-std=c++17', '-g', '-O1', '-pthread', sanitizer,
                    '-fno-omit-frame-pointer', '-I' + str(root), str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], timeout=60, check=True)
