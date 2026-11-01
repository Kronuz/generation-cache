#!/usr/bin/env python3
"""Breaks generation_cache.hh in a number of ways and requires the tests to fail on each.

usage: tools/mutants.py [-j N] [-k PATTERN] [CXX]

Each mutant replaces one piece of the header (the Nth occurrence of a text), builds the tests
that apply to it in a temporary directory and runs them. A mutant is killed when a test fails to
compile, aborts, or runs past the time limit (an endless probe). A mutant that survives is a hole
in the tests. A mutant of the portable word group builds with GENERATION_CACHE_NO_SIMD; one of the
SSE2 or NEON group builds only where that group is the platform's.
"""
import argparse
import concurrent.futures
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TESTS = ["group", "model", "first_insert", "exception_safety", "differential", "test"]
LIMIT = 120  # seconds per test

FIND_TAIL = "g = (g + step) & group_mask_;\n\t\t\t}\n\t\t}\n\t\tvoid insert_or_assign"
GROW_MOVE = "if constexpr (std::is_nothrow_move_constructible_v<Key> && std::is_nothrow_move_constructible_v<Value>) {"

# (name, variant, old text, new text, platform macro the mutant needs or "")
MUTANTS = [
    ("find skips the key compare", "simd", "if (e.first == key) return &e.second;", "if (true) return &e.second;", ""),
    ("find never stops at an empty slot", "simd", "if (probe.equal(ctrl_empty)) return nullptr;\n\t\t\t\tg = (g + step) & group_mask_;\n\t\t\t}\n\t\t}\n\t\tvoid insert_or_assign", "g = (g + step) & group_mask_;\n\t\t\t}\n\t\t}\n\t\tvoid insert_or_assign", ""),
    ("find gives up after one group", "simd", FIND_TAIL, "return nullptr;\n\t\t\t}\n\t\t}\n\t\tvoid insert_or_assign", ""),
    ("find reads an empty table", "simd", "if (!size_) return nullptr;\n\t\t\tconst std::uint64_t h = mix(key);\n\t\t\tconst std::uint8_t tag = tag_of(h);\n\t\t\tstd::size_t g = static_cast<std::size_t>(h) & group_mask_;\n\t\t\tfor (std::size_t step = 1;; ++step) {\n\t\t\t\tconst group probe(ctrl_ + g * width);\n\t\t\t\tfor (auto match = probe.equal(tag); match; match.clear_first()) {\n\t\t\t\t\tassert(", "const std::uint64_t h = mix(key);\n\t\t\tconst std::uint8_t tag = tag_of(h);\n\t\t\tstd::size_t g = static_cast<std::size_t>(h) & group_mask_;\n\t\t\tfor (std::size_t step = 1;; ++step) {\n\t\t\t\tconst group probe(ctrl_ + g * width);\n\t\t\t\tfor (auto match = probe.equal(tag); match; match.clear_first()) {\n\t\t\t\t\tassert(", ""),
    ("the probe step is constant", "simd", FIND_TAIL, "g = (g + 0) & group_mask_;\n\t\t\t}\n\t\t}\n\t\tvoid insert_or_assign", ""),
    ("insert never finds an existing key", "simd", "if (e.first == key) {\n\t\t\t\t\t\t\t// Overwrite", "if (false) {\n\t\t\t\t\t\t\t// Overwrite", ""),
    ("overwrite keeps the old value", "simd", "e.second = std::move(value);\n\t\t\t\t\t\t\treturn;", "return;", ""),
    ("insert ignores the room left", "simd", "if (room_) {\n\t\t\t\t\t\t\tplace(", "if (true) {\n\t\t\t\t\t\t\tplace(", ""),
    ("the table can fill completely", "simd", "return capacity - (capacity / 8 ? capacity / 8 : 1);", "return capacity;", ""),
    ("a table below 8 slots keeps no spare slot", "simd", "return capacity - (capacity / 8 ? capacity / 8 : 1);", "return capacity - capacity / 8;", ""),
    ("grow forgets to publish a control byte", "simd", "ctrl[to] = tag_of(h);", "(void)tag_of(h);", ""),
    ("padding reads as empty", "simd", "std::memset(ctrl + capacity, ctrl_padding, width - capacity);", "std::memset(ctrl + capacity, ctrl_empty, width - capacity);", ""),
    ("too few groups", "simd", "return (capacity < width ? 1 : capacity / width) - 1;", "return (capacity < width ? 1 : capacity / (width * 2)) - 1;", ""),
    ("clear keeps the size", "simd", "release();\n\t\t\tsize_ = 0;\n\t\t\troom_ = 0;", "release();\n\t\t\troom_ = 0;", ""),
    ("release leaks the entries", "simd", "\t\t\tdestroy_entries(ctrl_, slots_, capacity_);\n\t\t\tfree_block(ctrl_);\n\t\t\tctrl_ = nullptr;", "\t\t\tfree_block(ctrl_);\n\t\t\tctrl_ = nullptr;", ""),
    ("destroy_entries destroys the empty slots", "simd", "if (!(ctrl[i] & ctrl_empty)) slots[i].~Entry();", "if (ctrl[i] & ctrl_empty) slots[i].~Entry();", ""),
    ("place publishes before it constructs", "simd", "::new (static_cast<void*>(slots_ + i)) Entry(key, std::move(value));\n\t\t\tctrl_[i] = tag;", "ctrl_[i] = tag;\n\t\t\t::new (static_cast<void*>(slots_ + i)) Entry(key, std::move(value));", ""),
    ("grow always moves", "simd", GROW_MOVE, "if constexpr (true) {", ""),
    ("grow always copies", "simd", GROW_MOVE, "if constexpr (false) {", ""),
    ("the tag takes 8 bits", "simd", "static_cast<std::uint8_t>(h >> 57)", "static_cast<std::uint8_t>(h >> 56)", ""),
    ("the hash is not mixed", "simd", "return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64);", "return a;", ""),
    ("the hash keeps only the low half of the product", "simd", "return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64);", "return static_cast<std::uint64_t>(product);", ""),
    ("clear does not advance the generation", "simd", "entries_.clear();\n\t\t++generation_;", "entries_.clear();", ""),
    ("try_insert ignores the generation", "simd", "if (generation_ != at_generation) return false;", "if (false) return false;", ""),
    ("SSE2 group compares for greater", "simd", "_mm_cmpeq_epi8(bytes, _mm_set1_epi8(static_cast<char>(value)))", "_mm_cmpgt_epi8(bytes, _mm_set1_epi8(static_cast<char>(value)))", "__SSE2__"),
    ("NEON mask narrows by 2", "simd", "vshrn_n_u16(vreinterpretq_u16_u8(vceqq_u8(bytes, vdupq_n_u8(value))), 4)", "vshrn_n_u16(vreinterpretq_u16_u8(vceqq_u8(bytes, vdupq_n_u8(value))), 2)", "__aarch64__"),
    ("word group uses the inexact zero-byte test", "nosimd", "return mask(~(((x & low7) + low7) | x | low7));", "return mask((x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL);", ""),
    ("word group index forgets the shift", "nosimd", "using mask = slot_mask<std::uint64_t, 3>;", "using mask = slot_mask<std::uint64_t, 0>;", ""),
    ("mask clear_first clears nothing", "simd", "void clear_first() noexcept { bits_ &= bits_ - 1; }", "void clear_first() noexcept { bits_ &= bits_ - 0; }", ""),
]


def predefined(cxx):
    return subprocess.run([cxx, "-dM", "-E", "-x", "c++", "/dev/null"], capture_output=True, text=True).stdout


def run_one(job):
    index, (name, variant, old, new, _), cxx = job
    header = (ROOT / "generation_cache.hh").read_text()
    if header.count(old) != 1:
        return index, name, "BROKEN", f"the replaced text occurs {header.count(old)} times"
    work = pathlib.Path(tempfile.mkdtemp(prefix=f"gcmut{index}-"))
    try:
        (work / "test").mkdir()
        (work / "generation_cache.hh").write_text(header.replace(old, new))
        for t in TESTS:
            shutil.copy(ROOT / "test" / f"{t}.cc", work / "test" / f"{t}.cc")
        flags = ["-std=c++17", "-O1", "-g", "-pthread", "-I", str(work)]
        if variant == "nosimd":
            flags.append("-DGENERATION_CACHE_NO_SIMD=1")
        for t in TESTS:
            exe = work / t
            build = subprocess.run([cxx] + flags + [str(work / "test" / f"{t}.cc"), "-o", str(exe)], capture_output=True, text=True)
            if build.returncode != 0:
                return index, name, "KILLED", f"{t}: does not compile"
            try:
                run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=LIMIT, cwd=work)
            except subprocess.TimeoutExpired:
                return index, name, "KILLED", f"{t}: timed out"
            if run.returncode != 0:
                return index, name, "KILLED", f"{t}: exit {run.returncode}"
        return index, name, "SURVIVED", ""
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=2)
    ap.add_argument("-k", default="", help="run only the mutants whose name contains this")
    ap.add_argument("cxx", nargs="?", default=os.environ.get("CXX", "c++"))
    opts = ap.parse_args()
    macros = predefined(opts.cxx)
    jobs = []
    for i, m in enumerate(MUTANTS):
        if opts.k and opts.k not in m[0]:
            continue
        if m[4] and m[4] not in macros:
            continue
        jobs.append((i, m, opts.cxx))
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=opts.j) as pool:
        for index, name, verdict, detail in pool.map(run_one, jobs):
            print(f"{verdict:9s} {name}" + (f"  [{detail}]" if detail else ""), flush=True)
            bad += verdict != "KILLED"
    print(f"{len(jobs) - bad} of {len(jobs)} mutants killed")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
