---@meta
---@diagnostic disable: lowercase-global
---@diagnostic disable: missing-return
---@diagnostic disable: args-after-dots

---@class defold_api.binary_pack
---High-performance binary table serialisation and deserialisation extension
---with native Defold userdata support and SHA-256 tamper protection.
binary_pack = {}

---Encodes a Lua table (including nested tables, primitives, and Defold userdata
---types such as hash, vector3, vector4, quat, and matrix4) into a compact
---binary string.
---@param tbl table The Lua table to encode.
---@return string payload The encoded binary string.
function binary_pack.encode(tbl) end

---Decodes a binary string created by binary_pack.encode back into its original
---Lua table and Defold userdata types.
---@param payload string The binary string to decode.
---@return table tbl The decoded Lua table.
function binary_pack.decode(payload) end

---Encodes a Lua table into a compact binary string and appends a 32-byte
---SHA-256 integrity checksum to detect tampering or corruption.
---@param tbl table The Lua table to encode.
---@return string payload_sha The encoded binary string with an appended 32-byte SHA-256 checksum.
function binary_pack.encode_sha(tbl) end

---Verifies the appended 32-byte SHA-256 checksum and decodes the binary string
---back into its original Lua table. Throws a Lua error if the data has been
---tampered with or corrupted.
---@param payload_sha string The SHA-256 protected binary string to verify and decode.
---@return table tbl The verified and decoded Lua table.
function binary_pack.decode_sha(payload_sha) end
