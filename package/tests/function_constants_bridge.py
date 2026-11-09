#!/usr/bin/env python3
"""Exercise the frontend's reflection objects through Swift's Metal bridge."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[2] / 'ios18/sources/AdrenoMetal/NativeBridgeMetalFrontend.mm'
text = source.read_text()
begin = text.index('@interface NBFunctionConstant :')
end = text.index('@end', text.index('@implementation NBFunctionConstant', begin)) + len('@end')

with tempfile.TemporaryDirectory(prefix='metal-constant-bridge-') as directory:
    root = Path(directory)
    (root / 'constants.mm').write_text(
        '#import <Foundation/Foundation.h>\n#import <Metal/Metal.h>\n' + text[begin:end] + '''
extern "C" NSDictionary *make_constants(void) {
    NBFunctionConstant *constant = [NBFunctionConstant new];
    constant.name = @"flag";
    constant.type = MTLDataTypeFloat;
    constant.index = 7;
    constant.required = YES;
    return @{@"flag": constant};
}
''')
    (root / 'constants.h').write_text(
        '#import <Foundation/Foundation.h>\nNSDictionary * _Nonnull make_constants(void);\n')
    (root / 'main.swift').write_text('''
import Foundation
import Metal

let constants = make_constants() as! [String: MTLFunctionConstant]
let constant = constants["flag"]!
precondition(constant.name == "flag")
precondition(constant.type == .float)
precondition(constant.index == 7)
precondition(constant.required)
print("PASS: Swift Metal dictionary cast and reflection fields")
''')
    subprocess.run(['xcrun', 'clang++', '-fobjc-arc', '-dynamiclib', '-framework', 'Metal',
                    '-framework', 'Foundation', root / 'constants.mm', '-o', root / 'libconstants.dylib'],
                   check=True)
    subprocess.run(['xcrun', 'swiftc', root / 'main.swift', '-import-objc-header', root / 'constants.h',
                    '-L', root, '-lconstants', '-Xlinker', '-rpath', '-Xlinker', root,
                    '-o', root / 'test'], check=True)
    subprocess.run([root / 'test'], check=True)
