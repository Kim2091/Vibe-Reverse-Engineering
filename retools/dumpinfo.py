#!/usr/bin/env python3
"""Analyze Windows minidump files (.dmp).

Sub-commands:

  info       Modules loaded at crash time + exception summary
  threads    All threads with registers resolved to module+offset
  stack      Stack walk for a single thread (return addresses, annotated values)
  stackscan  Scan thread stack for code addresses, grouped by module
  exception  Exception record with MSVC C++ type decoding
  read       Read typed data from dump memory
  strings    Extract readable strings from dump memory
  memscan    Search dump memory for byte pattern or text
  memmap     List all captured memory regions with module affiliation
  diagnose   One-shot crash analysis pipeline

Usage:
    python -m retools.dumpinfo <dumpfile> info
    python -m retools.dumpinfo <dumpfile> threads
    python -m retools.dumpinfo <dumpfile> stack <thread_id>
    python -m retools.dumpinfo <dumpfile> stackscan <thread_id> [--module name]
    python -m retools.dumpinfo <dumpfile> exception
    python -m retools.dumpinfo <dumpfile> read <address> <type>
    python -m retools.dumpinfo <dumpfile> strings [--pattern regex]
    python -m retools.dumpinfo <dumpfile> memscan <pattern>
    python -m retools.dumpinfo <dumpfile> memmap
    python -m retools.dumpinfo <dumpfile> diagnose [--binary path]

Requires: pip install minidump pefile
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from minidump.minidumpfile import MinidumpFile
except ImportError:
    sys.exit("minidump not installed. Run: pip install minidump")

import pefile


def _load_dump(path: str) -> MinidumpFile:
    import logging
    logging.disable(logging.CRITICAL)
    try:
        return MinidumpFile.parse(path)
    finally:
        logging.disable(logging.NOTSET)


def _build_module_map(dump: MinidumpFile):
    """Return sorted list of (base, size, name) for address resolution."""
    modules = []
    if dump.modules:
        for m in dump.modules.modules:
            modules.append((m.baseaddress, m.size, m.name))
    return sorted(modules, key=lambda x: x[0])


def _resolve_addr(modules, addr: int) -> str:
    """Resolve an address to module+offset, or return hex string."""
    for base, size, name in modules:
        if base <= addr < base + size:
            mod_name = name.rsplit("\\", 1)[-1].rsplit("/", 1)[-1]
            return f"{mod_name}+0x{addr - base:X}"
    return f"0x{addr:X}"


def _read_dump_memory(dump: MinidumpFile, addr: int, size: int) -> bytes:
    """Read bytes from the dump's memory image."""
    reader = dump.get_reader()
    try:
        return reader.read(addr, size)
    except Exception:
        return b""


def _read_dump_chunked(dump: MinidumpFile, addr: int, total: int,
                       chunk: int = 4096) -> bytes:
    """Read a large range from dump memory, tolerating gaps between segments."""
    reader = dump.get_reader()
    parts = []
    for off in range(0, total, chunk):
        try:
            parts.append(reader.read(addr + off, min(chunk, total - off)))
        except Exception:
            parts.append(b"\x00" * min(chunk, total - off))
    return b"".join(parts)


def _get_exception_info(dump: MinidumpFile):
    """Extract exception record fields from the first exception record."""
    if not dump.exception or not dump.exception.exception_records:
        return None
    stream = dump.exception.exception_records[0]
    rec = stream.ExceptionRecord
    code_raw = rec.ExceptionCode_raw if hasattr(rec, "ExceptionCode_raw") else int(rec.ExceptionCode)
    return {
        "thread_id": stream.ThreadId,
        "code": code_raw,
        "address": rec.ExceptionAddress,
        "num_params": rec.NumberParameters,
        "params": list(rec.ExceptionInformation[:rec.NumberParameters]),
    }


def _resolve_msvc_exception(modules, dump, exc_info) -> str | None:
    """Try to decode MSVC C++ exception type from _ThrowInfo."""
    if exc_info["code"] != 0xE06D7363:
        return None
    params = exc_info["params"]
    if len(params) < 4:
        return None

    throw_info_param = params[2]
    image_base_param = params[3] if len(params) > 3 else 0

    target_base = image_base_param if image_base_param else exc_info["address"]
    for base, size, mod_path in _build_module_map(dump):
        if base <= target_base < base + size:
            try:
                pe = pefile.PE(mod_path, fast_load=False)
            except Exception:
                continue
            pe_base = pe.OPTIONAL_HEADER.ImageBase
            is_64 = pe.OPTIONAL_HEADER.Magic == 0x20B

            if is_64:
                runtime_base = image_base_param if image_base_param else base
                ti_rva = throw_info_param - runtime_base
                if ti_rva <= 0:
                    continue
                try:
                    data = pe.get_data(ti_rva, 24)
                    cta_rva = struct.unpack_from("<I", data, 12)[0]
                    if cta_rva == 0:
                        continue
                    cta_data = pe.get_data(cta_rva, 8)
                    n_types = struct.unpack_from("<I", cta_data, 0)[0]
                    types = []
                    for i in range(n_types):
                        ct_rva = struct.unpack_from(
                            "<I", pe.get_data(cta_rva + 4 + i * 4, 4), 0)[0]
                        ct_data = pe.get_data(ct_rva, 28)
                        td_rva = struct.unpack_from("<I", ct_data, 4)[0]
                        td_data = pe.get_data(td_rva, 64)
                        name_bytes = td_data[16:]
                        name = name_bytes.split(b"\x00", 1)[0].decode(
                            "ascii", errors="replace")
                        types.append(name)
                    return "; ".join(types) if types else None
                except Exception:
                    continue
            else:
                ti_va = throw_info_param
                ti_rva = ti_va - pe_base
                try:
                    data = pe.get_data(ti_rva, 16)
                except Exception:
                    continue
                cta_va = struct.unpack_from("<I", data, 12)[0]
                cta_rva = cta_va - pe_base
                try:
                    cta_data = pe.get_data(cta_rva, 8)
                except Exception:
                    continue
                n_types = struct.unpack_from("<I", cta_data, 0)[0]
                types = []
                for i in range(n_types):
                    ct_va = struct.unpack_from(
                        "<I", pe.get_data(cta_rva + 4 + i * 4, 4), 0)[0]
                    ct_rva = ct_va - pe_base
                    ct_data = pe.get_data(ct_rva, 20)
                    td_va = struct.unpack_from("<I", ct_data, 4)[0]
                    td_rva = td_va - pe_base
                    td_data = pe.get_data(td_rva, 64)
                    name_bytes = td_data[12:]
                    name = name_bytes.split(b"\x00", 1)[0].decode(
                        "ascii", errors="replace")
                    types.append(name)
                return "; ".join(types) if types else None
    return None


def cmd_info(dump: MinidumpFile, _args):
    modules = _build_module_map(dump)
    print(f"Modules ({len(modules)}):\n")
    for base, size, name in modules:
        short = name.rsplit("\\", 1)[-1].rsplit("/", 1)[-1]
        print(f"  0x{base:016X}  {size:10d}  {short}")

    exc = _get_exception_info(dump)
    if exc:
        print(f"\nException: code=0x{exc['code']:08X} "
              f"at {_resolve_addr(modules, exc['address'])} "
              f"(thread {exc['thread_id']})")


def cmd_threads(dump: MinidumpFile, args):
    modules = _build_module_map(dump)
    if not dump.threads:
        print("No thread information.")
        return

    exc_tid = None
    exc = _get_exception_info(dump)
    if exc:
        exc_tid = exc["thread_id"]

    verbose = getattr(args, "verbose", False)

    if not verbose:
        print(f"{'TID':>8s}  {'IP Location':<40s}  Notes")
        print(f"{'---':>8s}  {'----------':<40s}  -----")

    for t in dump.threads.threads:
        tid = t.ThreadId
        ctx = t.ContextObject
        if ctx is None:
            if verbose:
                print(f"\nThread {tid}: no context")
            continue

        if hasattr(ctx, "Rip"):
            ip = ctx.Rip
        elif hasattr(ctx, "Eip"):
            ip = ctx.Eip
        else:
            continue

        resolved = _resolve_addr(modules, ip)
        marker = " << EXCEPTION" if tid == exc_tid else ""

        if not verbose:
            print(f"{tid:>8d}  {resolved:<40s}{marker}")
            continue

        print(f"\nThread {tid}:{marker}")
        if hasattr(ctx, "Rip"):
            print(f"  RIP = 0x{ctx.Rip:016X}  ({resolved})")
            print(f"  RSP = 0x{ctx.Rsp:016X}  RBP = 0x{ctx.Rbp:016X}")
            print(f"  RAX = 0x{ctx.Rax:016X}  RBX = 0x{ctx.Rbx:016X}  "
                  f"RCX = 0x{ctx.Rcx:016X}  RDX = 0x{ctx.Rdx:016X}")
            print(f"  RSI = 0x{ctx.Rsi:016X}  RDI = 0x{ctx.Rdi:016X}  "
                  f"R8  = 0x{ctx.R8:016X}  R9  = 0x{ctx.R9:016X}")
        else:
            print(f"  EIP = 0x{ctx.Eip:08X}  ({resolved})")
            print(f"  ESP = 0x{ctx.Esp:08X}  EBP = 0x{ctx.Ebp:08X}")
            print(f"  EAX = 0x{ctx.Eax:08X}  EBX = 0x{ctx.Ebx:08X}  "
                  f"ECX = 0x{ctx.Ecx:08X}  EDX = 0x{ctx.Edx:08X}")


def cmd_stack(dump: MinidumpFile, args):
    modules = _build_module_map(dump)
    tid = int(args.thread_id)
    thread = None
    if dump.threads:
        for t in dump.threads.threads:
            if t.ThreadId == tid:
                thread = t
                break
    if thread is None:
        print(f"Thread {tid} not found.")
        return

    ctx = thread.ContextObject
    if ctx is None:
        print("No context for this thread.")
        return

    is_64 = hasattr(ctx, "Rip")
    sp = ctx.Rsp if is_64 else ctx.Esp
    ptr_size = 8 if is_64 else 4
    ptr_fmt = "<Q" if is_64 else "<I"
    w = 16 if is_64 else 8
    depth = int(args.depth) if hasattr(args, "depth") and args.depth else 64

    exec_ranges = set()
    for base, size, name in modules:
        try:
            pe = pefile.PE(name, fast_load=True)
            for s in pe.sections:
                if s.Characteristics & 0x20000000:
                    sec_va = base + s.VirtualAddress
                    exec_ranges.add((sec_va, sec_va + s.Misc_VirtualSize))
        except Exception:
            exec_ranges.add((base, base + size))

    def _in_code(addr):
        return any(lo <= addr < hi for lo, hi in exec_ranges)

    print(f"Stack walk for thread {tid} (SP=0x{sp:0{w}X}):\n")
    stack_data = _read_dump_memory(dump, sp, depth * ptr_size)
    if not stack_data:
        print("  (stack memory not available in dump)")
        return

    for i in range(0, len(stack_data) - ptr_size + 1, ptr_size):
        val = struct.unpack_from(ptr_fmt, stack_data, i)[0]
        addr = sp + i
        resolved = _resolve_addr(modules, val) if _in_code(val) else ""
        tag = " <-- RET" if resolved and "+" in resolved else ""
        if resolved:
            print(f"  0x{addr:0{w}X}: 0x{val:0{w}X}  {resolved}{tag}")
        else:
            print(f"  0x{addr:0{w}X}: 0x{val:0{w}X}")


def _get_thread(dump: MinidumpFile, tid: int):
    if dump.threads:
        for t in dump.threads.threads:
            if t.ThreadId == tid:
                return t
    return None


# OS modules that clutter stack scans (matched case-insensitively)
_OS_MODULES = frozenset([
    "ntdll.dll", "kernel32.dll", "kernelbase.dll", "win32u.dll",
    "user32.dll", "gdi32.dll", "ucrtbase.dll", "msvcrt.dll",
    "advapi32.dll", "combase.dll", "rpcrt4.dll", "sechost.dll",
    "bcryptprimitives.dll", "msvcp_win.dll",
])


def cmd_stackscan(dump: MinidumpFile, args):
    modules = _build_module_map(dump)
    tid = int(args.thread_id)
    thread = _get_thread(dump, tid)
    if thread is None:
        print(f"Thread {tid} not found.")
        return

    ctx = thread.ContextObject
    if ctx is None:
        print("No context for this thread.")
        return

    is_64 = hasattr(ctx, "Rip")
    sp = ctx.Rsp if is_64 else ctx.Esp
    ptr_size = 8 if is_64 else 4
    ptr_fmt = "<Q" if is_64 else "<I"
    w = 16 if is_64 else 8
    depth = args.depth

    mod_filter = args.module.lower() if args.module else None

    by_module = {}
    stack_data = _read_dump_chunked(dump, sp, depth * ptr_size)

    for i in range(0, len(stack_data) - ptr_size + 1, ptr_size):
        val = struct.unpack_from(ptr_fmt, stack_data, i)[0]
        for base, size, path in modules:
            if base <= val < base + size:
                short = path.rsplit("\\", 1)[-1].rsplit("/", 1)[-1]
                offset = val - base
                stack_addr = sp + i
                if mod_filter and short.lower() != mod_filter:
                    continue
                by_module.setdefault(short, []).append((stack_addr, offset))
                break

    if not by_module:
        print(f"No code addresses found in {depth} stack slots "
              f"from thread {tid}.")
        return

    print(f"Stack scan for thread {tid} (SP=0x{sp:0{w}X}, "
          f"{depth} slots):\n")

    for mod_name in sorted(by_module.keys()):
        if not mod_filter and mod_name.lower() in _OS_MODULES:
            continue
        entries = by_module[mod_name]
        print(f"  {mod_name} ({len(entries)} hits):")
        for stack_addr, offset in entries:
            print(f"    0x{stack_addr:0{w}X}: +0x{offset:X}")
        print()

    if not mod_filter:
        os_total = sum(
            len(v) for k, v in by_module.items() if k.lower() in _OS_MODULES
        )
        if os_total:
            print(f"  (+ {os_total} OS module hits hidden, "
                  f"use --module to show specific)")


def cmd_exception(dump: MinidumpFile, _args):
    modules = _build_module_map(dump)
    exc = _get_exception_info(dump)
    if not exc:
        print("No exception record in dump.")
        return

    print(f"Exception code:    0x{exc['code']:08X}")
    print(f"Exception address: 0x{exc['address']:016X}  "
          f"({_resolve_addr(modules, exc['address'])})")
    print(f"Thread ID:         {exc['thread_id']}")
    print(f"Parameters ({exc['num_params']}):")
    for i, p in enumerate(exc["params"][:exc["num_params"]]):
        print(f"  [{i}] 0x{p:016X}")

    if exc["code"] == 0xE06D7363:
        print("\nMSVC C++ exception (_CxxThrowException)")
        type_name = _resolve_msvc_exception(modules, dump, exc)
        if type_name:
            print(f"  Type: {type_name}")
        else:
            print("  (could not resolve type -- module/PE not accessible)")


def _iter_segments(dump: MinidumpFile):
    """Yield (start_addr, size) for every memory segment in the dump."""
    if dump.memory_segments:
        for seg in dump.memory_segments.memory_segments:
            yield seg.start_virtual_address, seg.size


def cmd_strings(dump: MinidumpFile, args):
    import re as _re
    modules = _build_module_map(dump)
    reader = dump.get_reader()
    pattern = _re.compile(args.pattern) if args.pattern else None
    min_len = args.min_len
    count = 0

    for seg_addr, seg_size in _iter_segments(dump):
        try:
            data = reader.read(seg_addr, seg_size)
        except Exception:
            continue

        i = 0
        while i < len(data):
            j = i
            while j < len(data) and 0x20 <= data[j] < 0x7F:
                j += 1
            slen = j - i
            if slen >= min_len and j < len(data) and data[j] == 0:
                s = data[i:j].decode("ascii", errors="replace")
                if pattern is None or pattern.search(s):
                    addr = seg_addr + i
                    mod = _resolve_addr(modules, addr)
                    loc = f"  ({mod})" if "+" in mod else ""
                    print(f"  0x{addr:016X} [{slen:4d}]: {s[:200]}{loc}")
                    count += 1
            i = j + 1 if j > i else i + 1

    if count == 0:
        print("No strings found matching criteria.")
    else:
        print(f"\n{count} strings found.")


def cmd_memscan(dump: MinidumpFile, args):
    modules = _build_module_map(dump)
    reader = dump.get_reader()
    raw = args.pattern

    if raw.startswith('"') and raw.endswith('"'):
        needle = raw[1:-1].encode("utf-8")
    else:
        try:
            needle = bytes.fromhex(raw.replace(" ", ""))
        except ValueError:
            needle = raw.encode("utf-8")

    print(f"Searching for {len(needle)} bytes: "
          f"{needle[:40]!r}{'...' if len(needle) > 40 else ''}\n")

    count = 0
    for seg_addr, seg_size in _iter_segments(dump):
        try:
            data = reader.read(seg_addr, seg_size)
        except Exception:
            continue

        idx = 0
        while True:
            idx = data.find(needle, idx)
            if idx < 0:
                break
            addr = seg_addr + idx
            mod = _resolve_addr(modules, addr)
            ctx_start = max(0, idx - 8)
            ctx_end = min(len(data), idx + len(needle) + 8)
            ctx = data[ctx_start:ctx_end]
            hex_str = " ".join(f"{b:02X}" for b in ctx)
            ascii_str = "".join(
                chr(b) if 0x20 <= b < 0x7F else "." for b in ctx)
            print(f"  0x{addr:016X} ({mod}):")
            print(f"    {hex_str}")
            print(f"    {ascii_str}")
            count += 1
            idx += 1

    if count == 0:
        print("Pattern not found in dump memory.")
    else:
        print(f"\n{count} matches found.")


def cmd_memmap(dump: MinidumpFile, _args):
    modules = _build_module_map(dump)
    total_size = 0
    seg_count = 0

    print("Memory regions in dump:\n")
    print(f"  {'Address':>18s}  {'Size':>10s}  Module")
    print(f"  {'-------':>18s}  {'----':>10s}  ------")

    for seg_addr, seg_size in _iter_segments(dump):
        mod = _resolve_addr(modules, seg_addr)
        mod_display = mod if "+" in mod else ""
        print(f"  0x{seg_addr:016X}  {seg_size:10,d}  {mod_display}")
        total_size += seg_size
        seg_count += 1

    print(f"\n{seg_count} segments, {total_size:,d} bytes total "
          f"({total_size / 1024 / 1024:.1f} MB)")


def cmd_diagnose(dump: MinidumpFile, args):
    modules = _build_module_map(dump)

    # --- 1. Exception info ---
    exc = _get_exception_info(dump)
    print("=== Exception ===\n")
    if not exc:
        print("  No exception record in dump.\n")
    else:
        print(f"  Code:    0x{exc['code']:08X}")
        print(f"  Address: 0x{exc['address']:016X} "
              f"({_resolve_addr(modules, exc['address'])})")
        print(f"  Thread:  {exc['thread_id']}")

        if exc["code"] == 0xE06D7363:
            type_name = _resolve_msvc_exception(modules, dump, exc)
            if type_name:
                print(f"  C++ type: {type_name}")

            # Try reading the thrown object (std::string for common patterns)
            params = exc["params"]
            if len(params) >= 2:
                obj_ptr = params[1]
                obj_data = _read_dump_memory(dump, obj_ptr, 64)
                if obj_data and len(obj_data) >= 32:
                    str_ptr = struct.unpack_from("<Q", obj_data, 0)[0]
                    str_len = struct.unpack_from("<Q", obj_data, 16)[0]
                    str_cap = struct.unpack_from("<Q", obj_data, 24)[0]
                    if 0 < str_len < 10000 and str_cap >= str_len:
                        if str_len <= 15:
                            # SSO: string data inline at offset 0
                            raw = obj_data[:str_len]
                            try:
                                msg = raw.decode("utf-8")
                                print(f"  Message (SSO): \"{msg}\"")
                            except Exception:
                                pass
                        else:
                            raw = _read_dump_memory(dump, str_ptr, str_len)
                            if raw and len(raw) == str_len:
                                try:
                                    msg = raw.decode("utf-8")
                                    print(f"  Message: \"{msg}\"")
                                except Exception:
                                    pass
                            else:
                                print(f"  String length={str_len}, "
                                      f"heap ptr=0x{str_ptr:X} "
                                      f"(not in dump)")
        print()

    # --- 2. Thread summary ---
    print("=== Threads ===\n")
    exc_tid = exc["thread_id"] if exc else None
    if dump.threads:
        for t in dump.threads.threads:
            ctx = t.ContextObject
            if ctx is None:
                continue
            if hasattr(ctx, "Rip"):
                ip = ctx.Rip
            elif hasattr(ctx, "Eip"):
                ip = ctx.Eip
            else:
                continue
            resolved = _resolve_addr(modules, ip)
            marker = " << EXCEPTION" if t.ThreadId == exc_tid else ""
            print(f"  {t.ThreadId:>8d}  {resolved}{marker}")
    print()

    # --- 3. Stack scan for exception thread ---
    if exc:
        print("=== Stack Scan (exception thread) ===\n")
        thread = _get_thread(dump, exc["thread_id"])
        if thread and thread.ContextObject:
            ctx = thread.ContextObject
            is_64 = hasattr(ctx, "Rip")
            sp = ctx.Rsp if is_64 else ctx.Esp
            w = 16 if is_64 else 8
            ptr_size = 8 if is_64 else 4
            ptr_fmt = "<Q" if is_64 else "<I"

            by_module = {}
            reader = dump.get_reader()
            for chunk_off in range(0, 8192 * ptr_size, 4096):
                try:
                    chunk = reader.read(sp + chunk_off, 4096)
                except Exception:
                    continue
                for i in range(0, len(chunk) - ptr_size + 1, ptr_size):
                    val = struct.unpack_from(ptr_fmt, chunk, i)[0]
                    for base, size, path in modules:
                        if base <= val < base + size:
                            short = (path.rsplit("\\", 1)[-1]
                                     .rsplit("/", 1)[-1])
                            if short.lower() in _OS_MODULES:
                                break
                            by_module.setdefault(short, []).append(
                                (sp + chunk_off + i, val - base))
                            break

            for mod_name in sorted(by_module.keys()):
                entries = by_module[mod_name]
                print(f"  {mod_name} ({len(entries)} frames):")
                for stack_addr, offset in entries[:20]:
                    print(f"    0x{stack_addr:0{w}X}: +0x{offset:X}")
                if len(entries) > 20:
                    print(f"    ... and {len(entries) - 20} more")
                print()
        else:
            print("  Exception thread not found or has no context.\n")

    # --- 4. Throw-site matching ---
    if args.binary:
        print("=== Throw-Site Match ===\n")
        try:
            from retools.throwmap import build_throw_map
        except ImportError:
            print("  throwmap module not available.\n")
            return

        pe, is_64, tmap = build_throw_map(args.binary)
        mapped = sum(1 for _, (_, s) in tmap.items() if s)
        print(f"  Binary: {args.binary}")
        print(f"  Throw sites: {len(tmap)} ({mapped} with strings)")

        binary_name = Path(args.binary).name.lower()
        runtime_base = None
        if dump.modules:
            for m in dump.modules.modules:
                mn = m.name.rsplit("\\", 1)[-1].rsplit("/", 1)[-1].lower()
                if mn == binary_name:
                    runtime_base = m.baseaddress
                    break

        if runtime_base is None:
            print(f"  Module '{binary_name}' not found in dump.\n")
            return

        ret_lookup = {}
        for site_rva, (insn_size, s) in tmap.items():
            ret_va = runtime_base + site_rva + insn_size
            ret_lookup[ret_va] = (site_rva, site_rva + insn_size, s)

        # Scan exception thread stack (chunked to handle segment gaps)
        matches = []
        if exc:
            thread = _get_thread(dump, exc["thread_id"])
            if thread and thread.ContextObject:
                ctx = thread.ContextObject
                sp = ctx.Rsp if hasattr(ctx, "Rsp") else ctx.Esp
                ptr_size = 8 if is_64 else 4
                ptr_fmt = "<Q" if is_64 else "<I"
                reader = dump.get_reader()
                for chunk_off in range(0, 65536, 4096):
                    try:
                        chunk = reader.read(sp + chunk_off, 4096)
                    except Exception:
                        continue
                    for i in range(0, len(chunk) - ptr_size + 1, ptr_size):
                        val = struct.unpack_from(ptr_fmt, chunk, i)[0]
                        if val in ret_lookup:
                            site_rva, ret_rva, s = ret_lookup[val]
                            matches.append(
                                (sp + chunk_off + i, site_rva, ret_rva, s))

        if matches:
            for stack_addr, site_rva, ret_rva, s in matches:
                display = s[:120] if s else "<string not resolved>"
                print(f"\n  MATCH: throw at +0x{site_rva:X} "
                      f"(ret +0x{ret_rva:X})")
                print(f"  Stack: 0x{stack_addr:016X}")
                print(f"  Error: \"{display}\"")
        else:
            print("\n  No throw-site matches on exception thread stack.")
        print()


def cmd_read(dump: MinidumpFile, args):
    addr = int(args.address, 16)
    type_map = {
        "uint8": ("<B", 1), "int8": ("<b", 1),
        "uint16": ("<H", 2), "int16": ("<h", 2),
        "uint32": ("<I", 4), "int32": ("<i", 4),
        "uint64": ("<Q", 8), "int64": ("<q", 8),
        "float": ("<f", 4), "double": ("<d", 8),
        "ptr32": ("<I", 4), "ptr64": ("<Q", 8),
    }
    if args.type == "bytes":
        size = int(args.count) if args.count else 64
        data = _read_dump_memory(dump, addr, size)
        if not data:
            print(f"Cannot read {size} bytes at 0x{addr:X}")
            return
        for off in range(0, len(data), 16):
            chunk = data[off : off + 16]
            hex_str = " ".join(f"{b:02X}" for b in chunk)
            ascii_str = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
            print(f"  0x{addr + off:016X}: {hex_str:<48s} {ascii_str}")
        return

    if args.type not in type_map:
        print(f"Unknown type '{args.type}'. "
              f"Valid: {', '.join(sorted(type_map))} bytes")
        return
    fmt, size = type_map[args.type]
    data = _read_dump_memory(dump, addr, size)
    if len(data) < size:
        print(f"Cannot read {size} bytes at 0x{addr:X}")
        return
    val = struct.unpack(fmt, data)[0]
    if isinstance(val, float):
        print(f"0x{addr:016X}: {val}")
    else:
        print(f"0x{addr:016X}: {val}  (0x{val:X})")


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("dumpfile", help="Path to minidump (.dmp) file")
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("info", help="Modules and exception summary")

    s = sub.add_parser("threads", help="All threads with registers")
    s.add_argument("--verbose", "-v", action="store_true",
                   help="Show full register dump per thread")

    s = sub.add_parser("stack", help="Stack walk for a thread")
    s.add_argument("thread_id", help="Thread ID (decimal)")
    s.add_argument("--depth", type=int, default=512,
                   help="Number of stack slots to scan (default: 512)")

    s = sub.add_parser("stackscan",
                       help="Scan thread stack for code addresses by module")
    s.add_argument("thread_id", help="Thread ID (decimal)")
    s.add_argument("--module", help="Filter to specific module name")
    s.add_argument("--depth", type=int, default=8192,
                   help="Stack slots to scan (default: 8192)")

    sub.add_parser("exception", help="Exception record with C++ type decoding")

    s = sub.add_parser("read", help="Read typed data from dump memory")
    s.add_argument("address", help="Virtual address in hex")
    s.add_argument("type",
                   help="Data type: uint8/16/32/64, int8/16/32/64, "
                        "float, double, ptr32, ptr64, bytes")
    s.add_argument("--count", help="Byte count (for 'bytes' type, default: 64)")

    s = sub.add_parser("strings", help="Extract strings from dump memory")
    s.add_argument("--pattern", help="Regex filter for strings")
    s.add_argument("--min-len", type=int, default=4, dest="min_len",
                   help="Minimum string length (default: 4)")

    s = sub.add_parser("memscan",
                       help="Search dump memory for byte/text pattern")
    s.add_argument("pattern",
                   help='Hex bytes ("48 65 6C 6C 6F") or '
                        'quoted text (\'"Hello"\')')

    sub.add_parser("memmap", help="List captured memory regions")

    s = sub.add_parser("diagnose",
                       help="One-shot crash analysis pipeline")
    s.add_argument("--binary",
                   help="Path to PE binary for throw-site matching")

    args = p.parse_args()
    dump = _load_dump(args.dumpfile)
    cmds = {
        "info": cmd_info, "threads": cmd_threads, "stack": cmd_stack,
        "stackscan": cmd_stackscan, "exception": cmd_exception,
        "read": cmd_read, "strings": cmd_strings, "memscan": cmd_memscan,
        "memmap": cmd_memmap, "diagnose": cmd_diagnose,
    }
    cmds[args.command](dump, args)


if __name__ == "__main__":
    main()
