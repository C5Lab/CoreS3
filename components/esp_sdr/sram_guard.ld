/* The modem owns the whole 64 KiB bank, including its IRAM alias. The heap
 * reservation in receiver.c excludes dynamic allocations; these assertions
 * also forbid statically linked code/data from reaching the bank. */
ASSERT(_iram_end <= 0x403c0000, "SDR snapshot bank overlaps IRAM")
ASSERT(_data_end <= 0x3fcd0000, "SDR snapshot bank overlaps data")
ASSERT(_bss_end <= 0x3fcd0000, "SDR snapshot bank overlaps BSS")
