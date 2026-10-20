#!/usr/bin/env python3
"""Check that the linked firmware really excludes the RF bank from the heap."""
import struct
import sys
from elftools.elf.elffile import ELFFile

with open(sys.argv[1] if len(sys.argv) > 1 else "build/coreS3.elf", "rb") as source:
    elf = ELFFile(source)
    table = elf.get_section_by_name(".symtab")
    symbols = {symbol.name: symbol["st_value"] for symbol in table.iter_symbols()}
    for name, limit in (("_iram_end", 0x403C0000), ("_data_end", 0x3FCD0000), ("_bss_end", 0x3FCD0000)):
        assert symbols[name] <= limit, f"{name} overlaps modem SRAM"
    start = symbols["soc_reserved_memory_region_start"]
    end = symbols["soc_reserved_memory_region_end"]
    raw = None
    for segment in elf.iter_segments():
        base = segment["p_vaddr"]
        if base <= start and end <= base + segment["p_filesz"]:
            raw = segment.data()[start-base:end-base]
            break
    assert raw is not None and len(raw) % 8 == 0, "Cannot read heap reservation table"
    regions = list(struct.iter_unpack("<II", raw))
    assert (0x3FCD0000, 0x3FCE0000) in regions, "Modem SRAM bank not reserved from heap"
print("PASS: RF SRAM bank reserved in ELF, static DRAM and IRAM do not overlap")
