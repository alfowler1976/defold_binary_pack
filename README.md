
# binary_pack

**`binary_pack`** is a high-performance, ultra-compact C++ binary table serialization extension for the [Defold](https://defold.com/ "null") game engine. Designed as a faster, smaller drop-in replacement for `sys.serialize()` and `sys.deserialize()`, it supports all standard Lua primitives alongside first-class Defold userdata (`hash`, `vector3`, `vector4`, `quat`, `matrix4`) and optional SHA-256 integrity verification.

While it performs similarly to MessagePack on general data, `binary_pack` includes further optimizations tailored specifically for **entity-style nested tables**  and native Defold math types. By automatically deduplicating repeated table keys into 2-byte references , caching decoded keys directly on the Lua stack, and compacting Defold userdata natively in C++, it cuts structured entity payloads by **~60% compared to MessagePack** (`27.5 KB` vs `70.5 KB` on 1,000 nested player records).

Size reduction and speed gains depend on the structure and types of data being serialized. On typical game workloads—such as nested entity tables, inventories, or mixed save data—`binary_pack.encode()` and `binary_pack.decode()` generally reduces payload sizes by **50% to 70%** (with higher savings on dense numeric or boolean arrays), while encoding and decoding roughly **1.5x to 4x faster** than `sys.serialize()` and `sys.deserialize()`.
For save files or sensitive payloads, `encode_sha()` and `decode_sha()` offer stronger data integrity and tamper detection at the cost of additional SHA-256 hashing overhead. (SHA versions are between **0.75x to 3x** on encoding and **0.5x to 1.5x** on decoding)

## Features

-   **Defold Userdata Support:** Natively encodes and decodes `hash`, `vmath.vector3`, `vmath.vector4`, `vmath.quat`, and `vmath.matrix4`—including `hash()` values used as table keys.
    
-   **Smart Userdata Compaction:**
    
    -   `vmath.vector3(0, 0, 0)` compacts from 13 bytes down to **1 byte** (`TYPE_VECTOR3_ZERO`).
        
    -   2D vectors `vmath.vector3(x, y, 0)` compact from 13 bytes down to **9 bytes** (`TYPE_VECTOR3_XY`).
        
    -   Identity quaternions `vmath.quat(0, 0, 0, 1)` compact from 17 bytes down to **1 byte** (`TYPE_QUAT_IDENTITY`).
        
-   **Key & Hash Deduplication:** Repeated string table keys (2–64 bytes) and `hash()` values are automatically deduplicated on the fly using an open-addressing lookup table on encode and a Lua stack cache on decode, replacing repeated keys and hashes with 2-byte references (`0..255`).
    
-   **Automatic Number & String Scaling:**
    
    -   Small integers (`0..127`) and short strings (`0..31` bytes) embed their value/length directly into a single-byte bitmask header.
        
    -   Larger numbers automatically scale to `uint8`, `int8`, `int16`, `int32`, lossless `float32`, or `float64`.
        
-   **Exact-Count Table Pre-Allocation:** Array and dictionary headers store exact element counts, allowing the decoder to pre-allocate exact Lua table capacities via `lua_createtable` with zero hash-table rehashing.
    
-   **Optional SHA-256 Integrity Verification:**  `binary_pack.encode_sha()` and `binary_pack.decode_sha()` append and verify a 32-byte cryptographic SHA-256 digest using `dmCrypt::HashSha256` to detect save-file corruption or casual tampering (trading some raw throughput for stronger data security).
    
-   **Memory & Safety Guards:** Reuses a persistent C++ buffer (up to 256 KB) to eliminate heap allocations across calls, while enforcing strict protections against non-table root inputs, circular table references, Lua stack overflows (max depth 64), `NaN`/`nil` keys, and truncated byte streams.

## Comparisons against sys.serialize and sys.deserialise



| Dataset | BinSize (B) | SysSize (B) | Saving | Enc Spd | Dec Spd | SHA Enc | SHA Dec | Verify |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| 10,000 ints (0-99) | 10,003 | 160,012 | 93.7% | 4.23x | 3.06x | 3.10x | 1.85x | PASS |
| 10,000 ints (1-10k) | 29,621 | 160,012 | 81.5% | 3.80x | 2.91x | 1.92x | 0.95x | PASS |
| 10,000 fractionals | 50,003 | 160,012 | 68.8% | 4.40x | 2.86x | 1.48x | 0.61x | PASS |
| 10,000 booleans | 10,003 | 70,012 | 85.7% | 4.55x | 2.94x | 3.17x | 1.63x | PASS |
| 1,000 short strings | 7,896 | 16,905 | 53.3% | 3.20x | 1.78x | 1.05x | 0.51x | PASS |
| 1,000 str-key ints | 10,516 | 23,616 | 55.5% | 1.94x | 1.28x | 0.80x | 0.51x | PASS |
| 10,000 str-key ints | 118,517 | 239,616 | 50.5% | 1.65x | 1.43x | 0.70x | 0.61x | PASS |
| 1,000 nested players | 27,490 | 84,013 | 67.3% | 2.62x | 2.11x | 1.36x | 1.14x | PASS |


## Installation

Add the repository URL to your `game.project` file under **Project → Dependencies**:


[https://github.com/alfowler1976/defold_binary_pack/archive/refs/tags/master.zip](https://github.com/alfowler1976/defold_binary_pack/archive/refs/tags/master.zip)


## API

All functions require a Lua `table` as the root value and throw a Lua error on failure (wrap calls in `pcall` to handle corrupt payloads or unsupported types safely).

* **`binary_pack.encode(tbl)`** — Encodes a Lua table into a compact binary string.
* **`binary_pack.decode(data)`** — Decodes a binary string back into a Lua table.
* **`binary_pack.encode_sha(tbl)`** — Encodes a Lua table and appends a 32-byte SHA-256 checksum for tamper and corruption detection.
* **`binary_pack.decode_sha(data)`** — Verifies the appended 32-byte SHA-256 checksum and decodes the binary string back into a Lua table. Throws an error if the checksum fails or the stream is corrupt.

