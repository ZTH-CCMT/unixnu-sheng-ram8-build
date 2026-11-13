#!/usr/bin/env python3
"""Extract staged production C functions, compile and run a Windows-only model."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
DEFAULT_SOURCE = HERE.parent / "source"
DEFAULT_CLANG = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang.exe")
DEFAULT_KERNEL32 = Path(r"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\um\x64\kernel32.lib")

def extract_function(path, name):
    text = path.read_text(encoding="utf-8")
    # Production definitions have the return type on the preceding line.
    match = re.search(r"(?m)^(?:static\s+)?(?:int|bool|uint64_t|void)\s*\n" + re.escape(name) + r"\s*\(", text)
    if not match:
        raise RuntimeError(f"Function definition not found: {path}:{name}")
    opening = text.find("{", match.end())
    if opening < 0:
        raise RuntimeError(f"Missing body: {path}:{name}")
    depth, state, i = 0, "code", opening
    while i < len(text):
        c, n = text[i], text[i+1:i+2]
        if state == "code":
            if c == "/" and n == "*": state, i = "block", i+1
            elif c == "/" and n == "/": state, i = "line", i+1
            elif c == '"': state = "string"
            elif c == "'": state = "char"
            elif c == "{": depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    body = text[match.start():i+1]
                    line = text.count("\n", 0, match.start()) + 1
                    return body, line
        elif state == "block":
            if c == "*" and n == "/": state, i = "code", i+1
        elif state == "line":
            if c == "\n": state = "code"
        elif state in ("string", "char"):
            if c == "\\": i += 1
            elif (state == "string" and c == '"') or (state == "char" and c == "'"): state = "code"
        i += 1
    raise RuntimeError(f"Unterminated body: {path}:{name}")

def negative_controls(base, generated_text, command, out):
    """Mutate only generated test copies; production sources are read-only."""
    prepare, _ = extract_function(base / "bind_vfsops.c", "bindfs_prepare_webkit_identity")
    anchor = '\terror = vnode_lookupat("Library", flags, &library, ctx, lowerrootvp);'
    if prepare.count(anchor) != 1 or prepare.count('\tvnode_put(lowerrootvp);\n') != 1:
        raise RuntimeError("Preparation negative-control anchor changed; review mutation before running")
    released = prepare.replace('\tvnode_put(lowerrootvp);\n', '').replace(anchor, '\tvnode_put(lowerrootvp);\n' + anchor)
    helper, _ = extract_function(base / "bind_vnops.c", "bindfs_webkit_native_lookup")
    gated = helper.replace("{\n", "{\n\tif (cnp->cn_flags & ISLASTCN) { return ENOENT; }\n", 1)
    controls = [("released-prepare-root", prepare, released, "nodes[SYSTEM].iocount>0"),
                ("last-component-gate", helper, gated, "selfcheck()==0")]
    results = []
    for name, original, replacement, expected in controls:
        if generated_text.count(original) != 1:
            raise RuntimeError(f"Non-unique negative control insertion: {name}")
        source = out / (name + ".c")
        executable = out / (name + ".exe")
        source.write_text(generated_text.replace(original, replacement, 1), encoding="utf-8", newline="\n")
        mutant_command = list(command)
        mutant_command[mutant_command.index(str(out / "webkit-v402-host-generated.c"))] = str(source)
        mutant_command[-1] = str(executable)
        built = subprocess.run(mutant_command, text=True, capture_output=True)
        (out / (name + "-compile.log")).write_text(built.stdout + built.stderr, encoding="utf-8")
        item = {"name": name, "compile_exit_code": built.returncode, "expected_assertion": expected}
        if built.returncode == 0:
            run = subprocess.run([str(executable)], text=True, capture_output=True)
            item.update(run_exit_code=run.returncode, stdout=run.stdout,
                        detected=run.returncode != 0 and expected in run.stdout)
            (out / (name + "-run.log")).write_text(run.stdout + run.stderr, encoding="utf-8")
        else:
            item["detected"] = False
        results.append(item)
        print(f"Negative control {name}: {'DETECTED' if item['detected'] else 'FAILED'}")
    return results

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE, help="XNU source root containing bsd/miscfs/bindfs")
    parser.add_argument("--output", type=Path, default=HERE / "out", help="Generated C, manifests, logs and executables directory")
    parser.add_argument("--clang", type=Path, default=DEFAULT_CLANG)
    parser.add_argument("--kernel32", type=Path, default=DEFAULT_KERNEL32)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This harness intentionally requires native Windows; WSL/Linux are not supported.")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    base = args.source.resolve() / "bsd" / "miscfs" / "bindfs"
    specs = [
        ("bind_vfsops.c", "bindfs_webkit_fileid"),
        ("bind_vfsops.c", "bindfs_prepare_webkit_identity"),
        ("bind_vnops.c", "bindfs_getattr"),
        ("bind_vnops.c", "bindfs_webkit_native_lookup"),
        ("bind_vnops.c", "bind_lookup"),
        ("bind_vfsops.c", "bindfs_webkit_identity_check"),
        ("bind_vfsops.c", "bindfs_vget"),
    ]
    chunks, manifest = [], []
    for filename, function in specs:
        path = base / filename
        body, line = extract_function(path, function)
        chunks.append(f'\n#line {line} "{path.as_posix()}"\n{body}\n')
        manifest.append({"source_path": str(path), "function": function, "line": line,
                         "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                         "extracted_sha256": hashlib.sha256(body.encode()).hexdigest()})
    preamble = HERE / "host-model.c"
    cases = HERE / "host-cases.c"
    generated = out / "webkit-v402-host-generated.c"
    generated.write_text(preamble.read_text(encoding="utf-8") + "".join(chunks)
                         + f'\n#line 1 "{cases.as_posix()}"\n' + cases.read_text(encoding="utf-8"),
                         encoding="utf-8", newline="\n")
    (out / "extraction-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    if args.generate_only:
        print(f"Generated {generated}")
        return 0
    executable = out / "webkit-v402-host.exe"
    command = [str(args.clang), "-std=c11", "-O0", "-Wall", "-Wextra",
               "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
               "-fno-builtin", "-fno-stack-protector", "-nostdlib", "-fuse-ld=lld",
               str(generated), str(args.kernel32), "-Xlinker", "/entry:mainCRTStartup",
               "-Xlinker", "/subsystem:console", "-o", str(executable)]
    compile_result = subprocess.run(command, text=True, capture_output=True)
    (out / "compile.log").write_text(compile_result.stdout + compile_result.stderr, encoding="utf-8")
    result = {"scope": "Windows host model; exact extracted C functions; no XNU kernel, APFS, real namei, MAC, namecache or LaunchServices execution",
              "command": command, "compile_exit_code": compile_result.returncode,
              "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
              "sources": manifest}
    if compile_result.returncode == 0:
        run = subprocess.run([str(executable)], text=True, capture_output=True)
        result.update(run_exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr)
        (out / "run.log").write_text(run.stdout + run.stderr, encoding="utf-8")
        print(run.stdout, end="")
        if run.stderr: print(run.stderr, end="", file=sys.stderr)
        rc = run.returncode
        if rc == 0:
            controls = negative_controls(base, generated.read_text(encoding="utf-8"), command, out)
            result["negative_controls"] = controls
            if not all(item["detected"] for item in controls): rc = 1
    else:
        print(compile_result.stdout + compile_result.stderr, file=sys.stderr)
        rc = compile_result.returncode
    (out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return rc

if __name__ == "__main__":
    raise SystemExit(main())
