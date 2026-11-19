# binary_pack

**`binary_pack`** is a C++ binary table serialization extension for the [Defold](https://defold.com/) game engine, designed specifically to be faster than `sys.serialize()` and `sys.deserialize()` while generating significantly smaller payloads. It serves as a direct drop-in replacement that supports all standard Lua primitives alongside native Defold userdata (`hash`, `vector3`, `vector4`, `quat`, `matrix4`) and optional SHA-256 integrity verification.

While it performs similarly to MessagePack on general data, `binary_pack` includes further optimizations tailored specifically for **entity-style nested tables** and native Defold math types. By automatically deduplicating repeated table keys into 2-byte references (`0..255`), caching decoded keys directly on the Lua stack, and compacting Defold userdata natively in C++, it cuts structured entity payloads by **~60% compared to MessagePack** (`27.5 KB` vs `70.5 KB` on 1,000 nested player records).

Across typical game workloads, standard `binary_pack.encode()` and `binary_pack.decode()` reduce payload sizes by **50% to 94%** while encoding **2x to 4.4x faster** and decoding **1.5x to 2.5x faster** than Defold's built-in serializer. For save files or sensitive payloads, `encode_sha()` and `decode_sha()` offer stronger data integrity and tamper detection at the cost of additional SHA-256 hashing overhead. (SHA versions are between **0.75x to 3x** on encoding and **0.5x to 1.5x** on decoding)

## Features

* **Defold Userdata Support:** Natively encodes and decodes `hash`, `vmath.vector3`, `vmath.vector4`, `vmath.quat`, and `vmath.matrix4`—including `hash()` values used as table keys.
* **Smart Userdata Compaction:**
* `vmath.vector3(0, 0, 0)` compacts from 13 bytes down to **1 byte** (`TYPE_VECTOR3_ZERO`).
* 2D vectors `vmath.vector3(x, y, 0)` compact from 13 bytes down to **9 bytes** (`TYPE_VECTOR3_XY`).
* Identity quaternions `vmath.quat(0, 0, 0, 1)` compact from 17 bytes down to **1 byte** (`TYPE_QUAT_IDENTITY`).


* **Key & Hash Deduplication:** Repeated string table keys (2–64 bytes) and `hash()` values are automatically deduplicated on the fly using an open-addressing lookup table on encode and a lazy Lua stack cache on decode, replacing repeated keys and hashes with 2-byte references (`0..255`).
* **Automatic Number & String Scaling:**
* Small integers (`0..127`) and short strings (`0..31` bytes) embed their value/length directly into a single-byte bitmask header.
* Larger numbers automatically scale to `uint8`, `int8`, `int16`, `int32`, lossless `float32`, or `float64`.


* **Exact-Count Table Pre-Allocation:** Array and dictionary headers store exact element counts, allowing the decoder to pre-allocate exact Lua table capacities via `lua_createtable` with zero hash-table rehashing.
* **Optional SHA-256 Integrity Verification:**  `binary_pack.encode_sha()` and `binary_pack.decode_sha()` append and verify a 32-byte cryptographic SHA-256 digest using `dmCrypt::HashSha256` to detect save-file corruption or casual tampering (trading some raw throughput for stronger data security).
* **Memory & Safety Guards:** Reuses a persistent C++ buffer (up to 256 KB) to eliminate heap allocations across calls, while enforcing strict protections against non-table root inputs, circular table references, Lua stack overflows (max depth 64), `NaN`/`nil` keys, and truncated byte streams.

## API

All functions require a Lua `table` as the root value and throw a Lua error on failure (wrap calls in `pcall` to handle corrupt payloads or unsupported types safely).

* **`binary_pack.encode(tbl)`** — Encodes a Lua table into a compact binary string.
* **`binary_pack.decode(data)`** — Decodes a binary string back into a Lua table.
* **`binary_pack.encode_sha(tbl)`** — Encodes a Lua table and appends a 32-byte SHA-256 checksum for tamper and corruption detection.
* **`binary_pack.decode_sha(data)`** — Verifies the appended 32-byte SHA-256 checksum and decodes the binary string back into a Lua table. Throws an error if the checksum fails or the stream is corrupt.
