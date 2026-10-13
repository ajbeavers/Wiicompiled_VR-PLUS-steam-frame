#!/usr/bin/env python3
"""Export the KartFrame game kit from a configured and built build dir.

The kit is everything the first-run build links except the translated game, plus the recipe that
compiles and links the player's own translation against it (modelled on upstream's Quest kit,
android/QuestGameKit.psm1). Nothing in it is derived from the disc. Compile flags and the link
line are read back out of the real CMake/ninja build graph (`ninja -t commands`, `-t compdb`), so
they cannot drift from a normal build.

    export-kit.py --build <build dir with kartframe_kit_probe built> --sysroot <make-sysroot.sh dir>
                  --out <kit dir>

Placeholders in kit.json: {kit} the kit directory, {sysroot} the kit's sysroot, {workspace} the
directory that holds the translator's generated/ tree. frame/make-kit.sh drives this.
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

SCHEMA = 1
ALLOWED_SYSTEM_LIBS = {"-ldl", "-lrt", "-lm", "-lpthread", "-lutil", "-pthread"}  # glibc stubs, stock
HERE = Path(__file__).resolve().parent


def die(msg):
    sys.exit(f"export-kit: error: {msg}")


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw).stdout


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cache_var(build, name):
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(name + ":"):
            return line.split("=", 1)[1]
    return None


def ninja_commands(build, target):
    return run(["ninja", "-C", str(build), "-t", "commands", target]).splitlines()


# ---- compile flags -------------------------------------------------------------------------

def reduce_flags(tokens, repo, kit_include_src, dropped):
    """Compile command tokens (compiler first) -> (kit flags, uses_pch)."""
    i = 1
    if tokens[0].endswith("ccache"):
        i = 2
    flags, pch = [], False
    toks = tokens
    while i < len(toks):
        a = toks[i]
        if a in ("-o", "-c", "-MT", "-MF"):
            i += 2
            continue
        if a in ("-MD", "-MMD", "-Winvalid-pch") or a == "-g":
            i += 1
            continue
        if a == "-isystem":
            dropped.add(toks[i + 1])
            i += 2
            continue
        if a == "-Xclang":
            if toks[i + 1] == "-include-pch":
                pch = True
                i += 4
                continue
            if toks[i + 1] == "-include":
                i += 4
                continue
            flags.append(a)
            i += 1
            continue
        if a.startswith("-I"):
            path = os.path.normpath(a[2:])
            if path == kit_include_src:
                flags.append("-I{kit}/include")
            elif path == str(repo):
                flags.append("-I{workspace}")
            else:
                dropped.add(path)
            i += 1
            continue
        if a.startswith("--sysroot="):
            i += 1
            continue
        if not a.startswith("-") and (a.endswith(".cpp") or a.endswith(".S") or a.endswith(".c")):
            i += 1  # the source itself
            continue
        flags.append(a)
        i += 1
    return flags, pch


def compile_kinds(build, repo):
    db = [e for e in json.loads(run(["ninja", "-C", str(build), "-t", "compdb"]))
          if e["file"].endswith((".cpp", ".S"))]
    patterns = {
        "translated": lambda f: "/build_shards/base_common/" in f or "/build_shards/base_sensitive/" in f,
        "product": lambda f: "/build_shards/base_registration/" in f or "/build_shards/base_dispatch/" in f,
        "runtime": lambda f: f.endswith("/data_sections_init.cpp") or f.endswith("/guest_symbol_table.cpp"),
        "asm": lambda f: f.endswith("data_sections_init_blobs_elf.S"),
    }
    kit_include_src = os.path.normpath(str(repo / "runtime/include"))
    out = {}
    for kind, match in patterns.items():
        entries = [e for e in db if match(e["file"])]
        if not entries:
            if kind == "translated":
                die("compile database has no translated shard; configure against a translation")
            die(f"compile database has no {kind} source")
        reduced = []
        for e in entries:
            dropped = set()
            flags, pch = reduce_flags(shlex.split(e["command"]), repo, kit_include_src, dropped)
            reduced.append((tuple(flags), pch, dropped, e))
        first = reduced[0]
        for r in reduced[1:]:
            if (r[0], r[1]) != (first[0], first[1]):
                die(f"{kind} sources do not share one flag set ({first[3]['file']} vs {r[3]['file']})")
        out[kind] = {"flags": list(first[0]), "pch": first[1], "sample": first[3]["file"],
                     "compiler": shlex.split(first[3]["command"])[0 if not first[3]["command"].startswith("ccache") else 1],
                     "dropped": sorted(first[2]), "count": len(entries)}
    return out


# ---- link recipe ---------------------------------------------------------------------------

def link_recipe(build, out, dawn_dir):
    lines = [l for l in ninja_commands(build, "kartframe_kit_probe") if " -o kartframe_kit_probe" in l]
    if len(lines) != 1:
        die("could not find the kartframe_kit_probe link command (build the target first)")
    line = lines[0]
    line = line.split(" && ", 1)[1] if line.startswith(": && ") else line
    line = line.split(" && ")[0]
    toks = shlex.split(line)
    toks = toks[1:]  # compiler
    flags, i = [], 0
    while i < len(toks) and not toks[i].endswith(".o"):
        t = toks[i]
        if t == "-Xlinker" and toks[i + 1].startswith("--dependency-file"):
            i += 2
            continue
        if t == "-Wl,--unresolved-symbols=ignore-all":
            i += 1
            continue
        flags.append(t)
        i += 1
    flags.insert(0, "--sysroot={sysroot}")

    copied = {}
    counter = [0]

    def vendor(path, sub):
        p = Path(path)
        if not p.is_absolute():
            p = build / p
        p = p.resolve()
        if p in copied:
            return copied[p]
        inside = str(p).startswith(str(build.resolve()) + "/") or (dawn_dir and str(p).startswith(str(dawn_dir) + "/"))
        if not inside:
            die(f"link input outside the build and the Dawn package (not self-contained): {p}")
        if p.suffix not in (".a", ".o"):
            die(f"link input is not a static archive or object: {p}")
        counter[0] += 1
        name = f"{counter[0]:03d}_{p.name}"
        dst = out / sub / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)
        copied[p] = f"{{kit}}/{sub}/{name}"
        return copied[p]

    objects, libs = [], []
    skip = False
    j = i
    while j < len(toks):
        t = toks[j]
        if t == "-o":
            j += 2
            continue
        if t.endswith(".o"):
            objects.append(vendor(t, "objects"))
        elif t.startswith("-l") or t == "-pthread":
            if t not in ALLOWED_SYSTEM_LIBS:
                die(f"link names a system library outside the stock set: {t}")
            libs.append(t)
        elif t.startswith("-"):
            libs.append(t)
        else:
            libs.append(vendor(t, "libs"))
        j += 1
    # Game slots: runtime-generated objects and product shards sit with the runtime objects, the
    # translated shards as an archive-like group before the libraries they call into.
    inputs = objects[:-1] + ["{game:runtime}", objects[-1], "{game:product}", "{game:translated}"] + libs
    return flags, inputs


# ---- resources ------------------------------------------------------------------------------

def collect_resources(build, out):
    """The files the game wants beside its executable: the POST_BUILD copies of the real product."""
    resdir = out / "resources"
    resdir.mkdir(parents=True, exist_ok=True)
    pat = re.compile(r"cmake -E (copy_directory|copy_if_different)\s+(\S+)\s+(\S+)\s*(?:&&|$)")
    seen = {}
    for line in ninja_commands(build, "WiiCompiled"):
        for kind, src, dst in pat.findall(line):
            dst = os.path.normpath(dst)
            if not dst.startswith(str(build.resolve()) + "/"):
                continue
            rel = os.path.relpath(dst, build.resolve())
            if rel.startswith("..") or rel == "kartframe":
                continue
            seen[rel] = (kind, src)
    if not seen:
        die("no POST_BUILD resource copies found for WiiCompiled")
    for rel, (kind, src) in sorted(seen.items()):
        d = resdir / rel
        if kind == "copy_directory":
            shutil.copytree(src, d, dirs_exist_ok=True, symlinks=True)
        else:
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, d)
    return sorted(seen)


def collect_licences(build, repo, out):
    lic = out / "licenses"
    lic.mkdir(parents=True, exist_ok=True)
    for name in ("LICENSE", "THIRD-PARTY-NOTICES.md"):
        if (repo / name).exists():
            shutil.copy2(repo / name, lic / name)
    if (repo / "licenses").is_dir():
        shutil.copytree(repo / "licenses", lic / "repo-licenses", dirs_exist_ok=True)
    pat = re.compile(r"^(LICEN[CS]E|COPYING|COPYRIGHT|NOTICE|UNLICENSE|FTL|GPLv2)", re.I)
    for src in sorted((build / "_deps").glob("*-src")):
        dst = lic / "third-party" / src.name[:-4]
        for cand in list(src.glob("*")) + list(src.glob("docs/*")) + list(src.glob("LICENSES/*")):
            if cand.is_file() and pat.match(cand.name):
                dst.mkdir(parents=True, exist_ok=True)
                shutil.copy2(cand, dst / cand.name)


# ---- fingerprint ----------------------------------------------------------------------------

def manifest(out):
    """sha256 lines for every kit file except kit.json and kit.sha256 itself (sorted by path)."""
    lines = []
    for p in sorted(out.rglob("*"), key=lambda x: str(x.relative_to(out)).encode()):
        rel = str(p.relative_to(out))
        if rel in ("kit.json", "kit.sha256"):
            continue
        if p.is_symlink():
            lines.append(f"{hashlib.sha256(os.readlink(p).encode()).hexdigest()}  {rel} -> {os.readlink(p)}")
        elif p.is_file():
            lines.append(f"{sha256_file(p)}  {rel}")
    return "\n".join(lines) + "\n"


def fingerprint(kit_json_text, manifest_text):
    """sha256 over kit.json without its fingerprint line, then the file manifest. setup.sh
    recomputes it with grep/sha256sum, so keep the two in step."""
    body = "".join(l for l in kit_json_text.splitlines(True) if not l.startswith('  "fingerprint"'))
    h = hashlib.sha256()
    h.update(hashlib.sha256(body.encode()).hexdigest().encode() + b"\n")
    h.update(hashlib.sha256(manifest_text.encode()).hexdigest().encode() + b"\n")
    return h.hexdigest()


# ---- validation -----------------------------------------------------------------------------

def validate(kinds, out, repo, sysroot_rel="sysroot"):
    """Compile one sample of every kind with exactly the flags the kit records. Fails if a flag set
    needs a header the kit does not carry, and if anything outside the kit, the sysroot, the
    workspace or the compiler's resource directory is read."""
    sysroot = out / sysroot_rel
    for kind, k in kinds.items():
        flags = [f.replace("{kit}", str(out)).replace("{workspace}", str(repo)) for f in k["flags"]]
        flags = ["--sysroot=" + str(sysroot)] + flags
        if k["pch"] or kind == "runtime":
            flags += ["-include", "mkw_pch.h"]
        comp = k["compiler"]
        if kind == "asm":
            cmd = [comp] + flags + ["-c", k["sample"], "-o", os.devnull]
        else:
            cmd = [comp] + flags + ["-H", "-fsyntax-only", k["sample"]]
        r = subprocess.run(cmd, text=True, capture_output=True)
        if r.returncode != 0:
            die(f"kit flags for '{kind}' do not compile {k['sample']}:\n{r.stderr[-1500:]}")
        if kind == "asm":
            continue
        resource = run([comp, "-print-resource-dir"]).strip()
        bad = set()
        for line in r.stderr.splitlines():
            m = re.match(r"^\.+ (\S+)$", line)
            if not m:
                continue
            h = os.path.realpath(m.group(1))
            ok = (str(out) in h or h.startswith(os.path.realpath(sysroot)) or h.startswith(resource)
                  or h.startswith(str(repo)) or h.startswith(os.path.realpath(str(repo)))
                  or h.startswith(os.path.realpath(os.path.dirname(k["sample"])) + "/"))
            if not ok:
                bad.add(h)
        if bad:
            die(f"'{kind}' compile reads files outside the kit: {sorted(bad)[:5]}")
        print(f"export-kit: validated {kind}: {Path(k['sample']).name}")


# ---- main -----------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-validate", action="store_true")
    a = ap.parse_args()
    build = Path(a.build).resolve()
    out = Path(a.out).resolve()
    sysroot = Path(a.sysroot).resolve()
    home = cache_var(build, "CMAKE_HOME_DIRECTORY")
    if not home:
        die("not a configured CMake build dir")
    repo = Path(home).resolve().parent
    dawn = cache_var(build, "MKW_DAWN_PACKAGE_DIR")
    dawn_dir = Path(dawn).resolve() if dawn else None
    if not (build / "kartframe_kit_probe").exists():
        die("kartframe_kit_probe is not built (ninja kartframe_kit_probe)")

    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    kinds = compile_kinds(build, repo)
    flags, inputs = link_recipe(build, out, dawn_dir)
    shutil.copytree(repo / "runtime/include", out / "include", symlinks=True)
    shutil.copytree(sysroot, out / "sysroot", symlinks=True)
    resources = collect_resources(build, out)
    collect_licences(build, repo, out)
    # What the translator reads besides the disc: the project description and symbol map, and the
    # runtime sources it scans for native overrides. The translation must match the runtime the kit
    # was built from, so the kit carries them.
    ws = out / "workspace"
    (ws / "projects/mkwii").mkdir(parents=True)
    for f in ("recomp.yml", "MAP.txt"):
        shutil.copy2(repo / "projects/mkwii" / f, ws / "projects/mkwii" / f)
    shutil.copytree(repo / "runtime/src", ws / "runtime/src", symlinks=True)
    shutil.copy2(HERE / "kitbuild.cmake", out / "kitbuild.cmake")
    clang_version = run([kinds["translated"]["compiler"], "--version"]).splitlines()[0]

    if not a.no_validate:
        validate(kinds, out, repo)

    tree = hashlib.sha256()
    for p in sorted((out / "include").rglob("*"), key=lambda x: str(x).encode()):
        if p.is_file():
            tree.update(str(p.relative_to(out / "include")).encode() + b" " + sha256_file(p).encode() + b"\n")

    def kind_json(k, extra=None):
        d = {"flags": k["flags"] + (["-Wa,--noexecstack"] if extra == "asm" else []), "pch": k["pch"]}
        return d

    kit = {
        "schema": SCHEMA,
        "target": "aarch64-unknown-linux-gnu",
        "compiler": clang_version,
        "runtimeIncludeFingerprint": tree.hexdigest(),
        "sysroot": "{kit}/sysroot",
        "workspace": "{kit}/workspace",
        "compile": {
            "translated": kind_json(kinds["translated"]),
            "product": kind_json(kinds["product"]),
            "runtime": kind_json(kinds["runtime"]),
            "asm": kind_json(kinds["asm"], "asm"),
        },
        "sources": {
            "runtime": ["{workspace}/generated/data_sections_init.cpp",
                        "{workspace}/generated/guest_symbol_table.cpp",
                        "{workspace}/generated/data_sections_init_blobs.S"],
            "product": ["@MKW_BASE_REGISTRATION_SOURCES"],
            "translated": ["@MKW_BASE_COMMON_SHARDS", "@MKW_BASE_PORTABLE_SENSITIVE_SHARDS"],
        },
        "link": {"output": "kartframe", "flags": flags, "inputs": inputs},
        "resources": resources,
    }
    # Report what the reduction dropped, so a surprise shows up in the log.
    for kind, k in kinds.items():
        print(f"export-kit: {kind}: {len(k['flags'])} flags, {k['count']} sources in the build, "
              f"pch={k['pch']}, dropped {len(k['dropped'])} include paths")

    man = manifest(out)
    (out / "kit.sha256").write_text(man)
    text = json.dumps(kit, indent=2).rstrip("\n")
    assert text.endswith("}")
    # the fingerprint is the last top-level key, on a line of its own
    text = text[:-1].rstrip() + ',\n  "fingerprint": "PLACEHOLDER"\n}\n'
    fp = fingerprint(text, man)
    text = text.replace("PLACEHOLDER", fp)
    assert fingerprint(text, man) == fp
    (out / "kit.json").write_text(text)
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file() and not p.is_symlink())
    print(f"export-kit: kit {out} ({size / 1048576:.0f} MiB) fingerprint {fp}")


if __name__ == "__main__":
    main()
