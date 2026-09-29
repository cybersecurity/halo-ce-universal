#!/usr/bin/env python3
"""Lift linked i386 ELF32 functions into native recomp C shards.

This consumes the final AOT guest ELF, not object files: all ordinary ELF
relocations have already been resolved by the linker, while the synthetic
host-import section remains a separate executable section and is mapped to
the import labels in the adjacent manifest.  Generated C retains the
XboxRecomp TLS register ABI (`void sub_VA(void)`).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import struct
import sys
import time
import traceback


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/macos-aot"
DEFAULT_ELF = BUILD / "halo_guest.elf"
DEFAULT_OUTPUT = BUILD / "lifted"
DEFAULT_RECOMP = ROOT / "tools/dependencies/xboxrecomp-runtime-overlay"


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def _cstring(blob: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(blob):
        return ""
    end = blob.find(b"\0", offset)
    if end < 0:
        return ""
    return blob[offset:end].decode("utf-8", errors="replace")


def parse_elf32(path: pathlib.Path) -> dict:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4:6] != b"\x01\x01":
        raise ValueError("expected little-endian ELFCLASS32")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    e_type, e_machine, entry = header[1], header[2], header[4]
    shoff, shentsize, shnum, shstrndx = header[6], header[11], header[12], header[13]
    if e_type != 2 or e_machine != 3:
        raise ValueError(f"expected linked i386 ET_EXEC, got type={e_type} machine={e_machine}")
    if shentsize < 40 or shoff + shentsize * shnum > len(data) or shstrndx >= shnum:
        raise ValueError("invalid ELF section table")
    raw_sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
                    for i in range(shnum)]
    shstr = raw_sections[shstrndx]
    shnames = data[shstr[4]:shstr[4] + shstr[5]]
    sections = []
    by_name = {}
    for index, item in enumerate(raw_sections):
        name = _cstring(shnames, item[0])
        section = {"index": index, "name": name, "type": item[1], "flags": item[2],
                   "addr": item[3], "offset": item[4], "size": item[5],
                   "link": item[6], "info": item[7], "align": item[8],
                   "entsize": item[9]}
        if item[1] != 8 and item[4] + item[5] > len(data):  # not SHT_NOBITS
            raise ValueError(f"section {name} extends beyond ELF bytes")
        sections.append(section)
        if name:
            by_name[name] = section
    text = by_name.get(".text")
    symtab = by_name.get(".symtab")
    strtab = sections[symtab["link"]] if symtab and symtab["link"] < len(sections) else None
    if not text or not symtab or not strtab:
        raise ValueError("linked ELF must contain .text, .symtab and its string table")
    strings = data[strtab["offset"]:strtab["offset"] + strtab["size"]]
    symbols = []
    entsize = symtab["entsize"] or 16
    if entsize < 16:
        raise ValueError("invalid ELF32 symbol entry size")
    for offset in range(symtab["offset"], symtab["offset"] + symtab["size"], entsize):
        if offset + 16 > len(data):
            break
        name_off, value, size, info, other, section_index = struct.unpack_from("<IIIBBH", data, offset)
        symbols.append({"name": _cstring(strings, name_off), "value": value, "size": size,
                        "type": info & 0xF, "bind": info >> 4,
                        "section_index": section_index})
    return {"data": data, "entry": entry, "sections": sections, "by_name": by_name,
            "text": text, "symbols": symbols}


def safe_ident(name: str) -> str:
    return name if name and name.replace("_", "a").isalnum() else ""


def load_runtime(recomp_root: pathlib.Path):
    sys.path.insert(0, str(recomp_root))
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator, BatchTranslator
    # Reject a previously imported overlay instead of hashing one path while
    # executing another. This is particularly important in reused interpreters.
    for name in ("tools.recomp.config", "tools.recomp.translator", "tools.recomp.lifter"):
        module_path = pathlib.Path(sys.modules[name].__file__).resolve()
        if not module_path.is_relative_to(recomp_root.resolve()):
            raise RuntimeError(f"runtime module {name} loaded outside selected overlay: {module_path}")
    return config, FunctionTranslator, BatchTranslator


def build_func_db(elf: dict, config) -> tuple[dict, dict, list[dict]]:
    text = elf["text"]
    text_index = text["index"]
    text_end = text["addr"] + text["size"]
    aliases: dict[int, list[dict]] = {}
    zero_size_func_symbols: dict[int, list[dict]] = {}
    for symbol in elf["symbols"]:
        if symbol["type"] != 2 or symbol["section_index"] != text_index:
            continue
        if not symbol["size"]:
            if text["addr"] <= symbol["value"] < text_end:
                zero_size_func_symbols.setdefault(symbol["value"], []).append(symbol)
            continue
        start, end = symbol["value"], symbol["value"] + symbol["size"]
        if start < text["addr"] or end > text_end or end <= start:
            continue
        aliases.setdefault(start, []).append(symbol)
    func_db = {}
    for start, syms in sorted(aliases.items()):
        end = min(text_end, max(s["value"] + s["size"] for s in syms))
        func_db[start] = {"_addr": start, "start": f"0x{start:08X}", "end": end,
                          "size": end - start, "name": f"sub_{start:08X}",
                          "section": ".text", "elf_aliases": sorted(
                          {s["name"] for s in syms if s["name"]})}

    # Some static-archive assembly helpers are exported as STT_FUNC symbols
    # with st_size == 0.  Calls to those exact starts otherwise become
    # unresolved direct calls even though the linked ELF carries a real symbol
    # and disassemblable body (notably the C fenv helpers and setjmp aliases).
    # Bound such a body by the next distinct STT_FUNC address in .text.  This
    # does not invent an interior entry: each start is explicit ELF metadata.
    # The translator still stops control flow at RET/JMP; the bound only keeps
    # decoding away from the next function.
    all_starts = sorted(set(aliases) | set(zero_size_func_symbols))
    next_start = {start: all_starts[i + 1] if i + 1 < len(all_starts) else text_end
                  for i, start in enumerate(all_starts)}
    for start, syms in sorted(zero_size_func_symbols.items()):
        if start in func_db:
            func_db[start]["elf_aliases"] = sorted(set(func_db[start]["elf_aliases"]) |
                                                    {s["name"] for s in syms if s["name"]})
            continue
        end = next_start[start]
        if end <= start:
            continue
        func_db[start] = {"_addr": start, "start": f"0x{start:08X}", "end": end,
                          "size": end - start, "name": f"sub_{start:08X}",
                          "section": ".text", "elf_aliases": sorted(
                              {s["name"] for s in syms if s["name"]}),
                          "inferred_zero_sized_symbol_end": True}

    imports = elf["by_name"].get(".host_imports")
    label_db = {}
    if imports:
        for symbol in elf["symbols"]:
            if (symbol["type"] == 2 and symbol["section_index"] == imports["index"]
                    and symbol["name"] and safe_ident(symbol["name"])):
                label_db[symbol["value"]] = symbol["name"]

    sections = []
    for section in elf["sections"]:
        if not section["name"] or not (section["flags"] & 0x2):  # SHF_ALLOC
            continue
        is_code = bool(section["flags"] & 0x4)  # SHF_EXECINSTR
        raw_size = 0 if section["type"] == 8 else section["size"]
        sections.append(config.Section(section["name"], section["addr"], section["size"],
                                       section["offset"], raw_size, is_code))
    return func_db, label_db, sections


def tools_provenance(recomp_root: pathlib.Path) -> dict:
    git = {}
    try:
        import subprocess
        result = subprocess.run(["git", "-C", str(recomp_root), "rev-parse", "HEAD"],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if result.returncode == 0:
            git["head"] = result.stdout.strip()
            status = subprocess.run(["git", "-C", str(recomp_root), "status", "--short"],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            git["status"] = status.stdout.splitlines()
    except Exception as error:
        git["error"] = str(error)
    files = {}
    for relative in ("tools/recomp/translator.py", "tools/recomp/lifter.py",
                     "tools/recomp/disasm.py", "tools/recomp/config.py",
                     "templates/runtime/recomp_types.h"):
        path = recomp_root / relative
        if path.is_file():
            files[relative] = digest(path)
    overlay_manifest_path = recomp_root / "halo-overlay-manifest.json"
    overlay_manifest = None
    if overlay_manifest_path.is_file():
        overlay_manifest = json.loads(overlay_manifest_path.read_text())
        files["halo-overlay-manifest.json"] = digest(overlay_manifest_path)
    return {"root": str(recomp_root), "git": git, "overlay_manifest": overlay_manifest,
            "effective_sha256": files,
            "loaded_modules": {
                name: str(pathlib.Path(sys.modules[name].__file__).resolve())
                for name in ("tools.recomp.config", "tools.recomp.translator",
                             "tools.recomp.lifter", "tools.recomp.disasm")
                if name in sys.modules}}


def emit_header(output: pathlib.Path, func_db: dict, label_db: dict, unresolved: dict,
                imports: list[dict]):
    names = {info["name"] for info in func_db.values()}
    names.update(label_db.values())
    names.update(f"sub_{item['va']:08X}" for item in imports)
    names.update(unresolved.values())
    lines = ["/* Generated from linked i386 guest symbols; do not edit. */",
             "#ifndef HALO_MACOS_RECOMP_FUNCS_H", "#define HALO_MACOS_RECOMP_FUNCS_H",
             '#include "recomp_types.h"', ""]
    lines.extend(f"void {name}(void);" for name in sorted(names))
    lines.extend(["", "#endif", ""])
    (output / "recomp_funcs.h").write_text("\n".join(lines))


def emit_image_symbols(output: pathlib.Path, elf: dict, elf_sha: str) -> dict:
    """Write the runtime's exact ELF entry/thread/init-array metadata."""
    wanted = ("__guest_start", "__guest_thread_start", "__guest_thread_attach", "__errno_location")
    values = {}
    for name in wanted:
        matches = [s["value"] for s in elf["symbols"]
                   if s["name"] == name and s["section_index"] == elf["text"]["index"]]
        if len(set(matches)) != 1:
            raise ValueError(f"expected one linked .text symbol for {name}, got {matches}")
        values[name] = matches[0]
    arrays = {}
    for name in (".init_array", ".fini_array"):
        section = elf["by_name"].get(name)
        if section:
            raw = elf["data"][section["offset"]:section["offset"] + section["size"]]
            if len(raw) % 4:
                raise ValueError(f"{name} byte length is not a multiple of pointer size")
            arrays[name] = {"va": section["addr"], "bytes": section["size"],
                            "entries": list(struct.unpack("<" + "I" * (len(raw) // 4), raw))}
        else:
            arrays[name] = {"va": 0, "bytes": 0, "entries": []}
    init, fini = arrays[".init_array"], arrays[".fini_array"]
    lines = [
        "/* Generated from the hash-pinned linked Halo ELF. */",
        "#ifndef HALO_RECOMP_IMAGE_SYMBOLS_H",
        "#define HALO_RECOMP_IMAGE_SYMBOLS_H",
        "#include <stdint.h>",
        f'#define HALO_RECOMP_IMAGE_SHA256 "{elf_sha}"',
        f"#define HALO_RECOMP_ENTRY_VA UINT32_C(0x{elf['entry']:08X})",
        f"#define HALO_RECOMP_GUEST_START_VA UINT32_C(0x{values['__guest_start']:08X})",
        f"#define HALO_RECOMP_THREAD_START_VA UINT32_C(0x{values['__guest_thread_start']:08X})",
        f"#define HALO_RECOMP_THREAD_ATTACH_VA UINT32_C(0x{values['__guest_thread_attach']:08X})",
        f"#define HALO_RECOMP_ERRNO_LOCATION_VA UINT32_C(0x{values['__errno_location']:08X})",
        f"#define HALO_RECOMP_INIT_ARRAY_VA UINT32_C(0x{init['va']:08X})",
        f"#define HALO_RECOMP_INIT_ARRAY_BYTES UINT32_C(0x{init['bytes']:X})",
        f"#define HALO_RECOMP_INIT_ARRAY_COUNT UINT32_C({len(init['entries'])})",
        f"#define HALO_RECOMP_FINI_ARRAY_VA UINT32_C(0x{fini['va']:08X})",
        f"#define HALO_RECOMP_FINI_ARRAY_BYTES UINT32_C(0x{fini['bytes']:X})",
        f"#define HALO_RECOMP_FINI_ARRAY_COUNT UINT32_C({len(fini['entries'])})",
        "#endif",
        "",
    ]
    (output / "recomp_image_symbols.h").write_text("\n".join(lines))
    return {"symbols": {"entry": elf["entry"], **values}, "init_array": init,
            "fini_array": fini}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=pathlib.Path, default=DEFAULT_ELF)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--recomp-root", type=pathlib.Path, default=DEFAULT_RECOMP)
    parser.add_argument("--import-manifest", type=pathlib.Path,
                        default=BUILD / "host-import-manifest.json")
    parser.add_argument("--expected-elf-sha256", default="",
                        help="required unless the import manifest pins the ELF hash")
    parser.add_argument("--chunk-size", type=int, default=256)
    parser.add_argument("--limit", type=int, default=0, help="lift only first N functions")
    parser.add_argument("--address", action="append", default=[],
                        help="lift selected guest VA (repeatable; accepts 0x prefix)")
    parser.add_argument("--discover", action="store_true",
                        help="run static callback/CFG recovery before lifting")
    args = parser.parse_args()
    if args.chunk_size < 1:
        parser.error("--chunk-size must be positive")
    elf_path = args.elf.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    elf = parse_elf32(elf_path)
    elf_sha = digest(elf_path)
    manifest = json.loads(args.import_manifest.read_text())
    expected_sha = args.expected_elf_sha256 or manifest.get("elf_sha256", "")
    if not expected_sha:
        raise SystemExit("import manifest has no elf_sha256; pass --expected-elf-sha256 for this frozen image")
    if elf_sha.lower() != expected_sha.lower():
        raise SystemExit(f"ELF hash mismatch: expected {expected_sha}, got {elf_sha}")
    config, FunctionTranslator, BatchTranslator = load_runtime(args.recomp_root.resolve())
    func_db, label_db, sections = build_func_db(elf, config)
    import_section = elf["by_name"].get(".host_imports")
    if not import_section:
        raise SystemExit("linked ELF has no .host_imports section")
    import_symbols = {s["name"]: s["value"] for s in elf["symbols"]
                      if s["type"] == 2 and s["section_index"] == import_section["index"] and s["name"]}
    expected_imports = manifest.get("imports", [])
    for item in expected_imports:
        if import_symbols.get(item["name"]) != item["va"]:
            raise SystemExit(f"import manifest mismatch for {item['name']}: "
                             f"ELF has {import_symbols.get(item['name'])!r}, manifest has {item['va']:#x}")
    if len(import_symbols) != len(expected_imports):
        raise SystemExit(f"import manifest has {len(expected_imports)} entries, "
                             f"ELF has {len(import_symbols)} import functions")
    image_symbols = emit_image_symbols(output, elf, elf_sha)
    config._install(sections, elf["entry"], 0, str(elf_path))
    translator = FunctionTranslator(elf["data"], func_db, label_db,
                                   seh_prolog=0, seh_epilog=0,
                                   setjmp_fn=0, longjmp_fn=0)
    if args.discover:
        translator.discover_static_indirect_targets()
        translator.discover_cfg_ownership()
        func_db = translator.func_db

    selected = sorted(func_db.items())
    if args.address:
        wanted = {int(value, 0) for value in args.address}
        missing = sorted(wanted - set(func_db))
        if missing:
            raise SystemExit("requested addresses are not lifted ELF functions: " +
                             ", ".join(f"0x{x:08X}" for x in missing))
        selected = [item for item in selected if item[0] in wanted]
    if args.limit:
        selected = selected[:args.limit]

    types_src = args.recomp_root.resolve() / "templates/runtime/recomp_types.h"
    types_text = types_src.read_text()
    import_base = int(manifest["base"])
    import_stride = int(manifest["stride"])
    import_limit = import_base + len(expected_imports) * import_stride
    old_icall_predicate = """#define RECOMP_ICALL_IS_CODE(_va) \\
    ((_va) >= 0xFE000000u || g_xbox_code_hi == 0u || \\
     ((_va) >= g_xbox_code_lo && (_va) < g_xbox_code_hi))"""
    new_icall_predicate = f"""#define RECOMP_ICALL_IS_CODE(_va) \\
    ((_va) >= 0xFE000000u || \\
     ((_va) >= 0x{import_base:08X}u && (_va) < 0x{import_limit:08X}u) || \\
     g_xbox_code_hi == 0u || \\
     ((_va) >= g_xbox_code_lo && (_va) < g_xbox_code_hi))"""
    if types_text.count(old_icall_predicate) != 1:
        raise SystemExit("runtime template indirect-call predicate changed; refusing unsafe token-range patch")
    types_text = types_text.replace(old_icall_predicate, new_icall_predicate)
    (output / "recomp_types.h").write_text(types_text)
    run_started = time.time()
    bodies = []
    failed = []
    unsupported = []
    function_starts = sorted(func_db)
    unimplemented_by_function = {address: start for start in function_starts
                                 for address in ()}
    del unimplemented_by_function
    for index, (address, info) in enumerate(selected, 1):
        before = {mnemonic: set(addrs) for mnemonic, addrs in translator.lifter.unimplemented.items()}
        try:
            code = translator.translate_function(address, info)
            if not code:
                raise RuntimeError("translator returned no body")
        except Exception as error:
            failed.append({"va": address, "name": info["name"],
                           "exception": f"{type(error).__name__}: {error}",
                           "traceback": traceback.format_exc(limit=5)})
            code = (f"void {info['name']}(void) {{ "
                    f"recomp_unsupported_instruction(0x{address:08X}u); }}")
        newly_unimplemented = []
        for mnemonic, addrs in translator.lifter.unimplemented.items():
            old = before.get(mnemonic, set())
            newly_unimplemented.extend((mnemonic, pc) for pc in addrs if pc not in old)
        if newly_unimplemented:
            unsupported.append({"va": address, "mnemonics": [
                {"mnemonic": mnemonic, "instruction_va": pc}
                for mnemonic, pc in sorted(newly_unimplemented, key=lambda x: x[1])]})
        bodies.append((address, info["name"], code))
        if index % 250 == 0 or index == len(selected):
            print(f"lifted {index}/{len(selected)} functions", file=sys.stderr)

    # Write each shard once; all generated function calls have declarations in
    # the shared header, even when the target is in a different shard.
    for shard_index in range((len(bodies) + args.chunk_size - 1) // args.chunk_size):
        chunk = bodies[shard_index * args.chunk_size:(shard_index + 1) * args.chunk_size]
        lines = [f"/* Generated ELF translation shard {shard_index}: {len(chunk)} functions. */",
                 "#define RECOMP_GENERATED_CODE", '#include "recomp_funcs.h"',
                 "#include <math.h>", ""]
        lines.extend(code for _, _, code in chunk)
        (output / f"recomp_{shard_index:04d}.c").write_text("\n\n".join(lines) + "\n")

    defined = {name for _, name, _ in bodies}
    import_names = set(label_db.values())
    unresolved = {address: name for address, name in translator.lifter.referenced_calls.items()
                  if name not in defined and name not in import_names}
    stub_lines = ["/* Fail-fast stubs for direct calls absent from linked function symbols. */",
                  "#define RECOMP_GENERATED_CODE", '#include "recomp_funcs.h"', ""]
    for address, name in sorted(unresolved.items()):
        stub_lines.append(f"void {name}(void) {{ recomp_icall_fail_log(0x{address:08X}u); }}")
    (output / "recomp_stubs_unresolved.c").write_text("\n".join(stub_lines) + "\n")

    # Named imports are called directly by translated guest code, while
    # indirect calls arrive at their synthetic ELF VAs. Give both paths a
    # typed `void(void)` adapter that dispatches the exact manifest token.
    import_lines = ["/* Generated fail-fast ABI aliases for ELF host imports. */",
                    "#define RECOMP_GENERATED_CODE", '#include "recomp_funcs.h"',
                    '#include "guest_import_registry.h"', ""]
    manual_items = [(address, name) for address, name, _ in bodies]
    for item in expected_imports:
        address, name = item["va"], item["name"]
        import_lines.extend([
            f"void sub_{address:08X}(void) {{ mac_guest_import_dispatch_token(0x{address:08X}u); }}",
            f"void {name}(void) {{ sub_{address:08X}(); }}",
        ])
        manual_items.append((address, f"sub_{address:08X}"))
    (output / "recomp_import_aliases.c").write_text("\n".join(import_lines) + "\n")

    manual_lines = ["/* Exact VA resolver for native/manual guest functions and imports. */",
                    '#include "recomp_funcs.h"', "#include <stddef.h>", "",
                    "typedef struct { uint32_t va; recomp_func_t func; } manual_entry;",
                    "static const manual_entry entries[] = {"]
    manual_lines.extend(f"    {{ 0x{address:08X}u, {name} }},"
                        for address, name in sorted(manual_items))
    manual_lines.extend(["};", "", "recomp_func_t recomp_lookup_manual(uint32_t xbox_va)",
                         "{", "    size_t lo = 0, hi = sizeof(entries) / sizeof(entries[0]);",
                         "    while (lo < hi) {", "        size_t mid = lo + (hi - lo) / 2;",
                         "        if (entries[mid].va < xbox_va) lo = mid + 1;",
                         "        else if (entries[mid].va > xbox_va) hi = mid;",
                         "        else return entries[mid].func;", "    }", "    return NULL;", "}", ""])
    (output / "recomp_manual_dispatch.c").write_text("\n".join(manual_lines))

    # Use the translator's canonical sorted VA dispatch generator so native
    # runtime lookup stays on the same convention as XboxRecomp outputs.
    batch = object.__new__(BatchTranslator)
    batch.translator = translator
    batch.output_dir = str(output)
    batch.title = "Halo AOT ELF"
    batch._write_dispatch_table(bodies, str(output / "recomp_dispatch.c"), "recomp_funcs.h")
    emit_header(output, func_db, label_db, unresolved, expected_imports)

    report = {
        "format": 1,
        "input_elf": str(elf_path),
        "input_elf_sha256": elf_sha,
        "expected_elf_sha256": expected_sha,
        "entry": f"0x{elf['entry']:08X}",
        "text": {"va": f"0x{elf['text']['addr']:08X}", "size": elf["text"]["size"]},
        "function_symbols_total": len(func_db),
        "functions_selected": len(selected),
        "functions_translated": len(bodies) - len(failed),
        "functions_failed": failed,
        "functions_with_unsupported_instructions": unsupported,
        "unresolved_direct_targets": [
            {"va": f"0x{address:08X}", "name": name}
            for address, name in sorted(unresolved.items())],
        "imports": [{"va": f"0x{address:08X}", "name": name}
                    for address, name in sorted(label_db.items())],
        "image_symbols": image_symbols,
        "shards": (len(bodies) + args.chunk_size - 1) // args.chunk_size,
        "chunk_size": args.chunk_size,
        "elapsed_seconds": round(time.time() - run_started, 3),
        "translator_provenance": tools_provenance(args.recomp_root.resolve()),
    }
    (output / "lift_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items()
                      if k not in ("functions_failed", "functions_with_unsupported_instructions",
                                   "unresolved_direct_targets", "imports", "translator_provenance")},
                     indent=2))
    print(f"output: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
