local M = {}

local function read_u16_le(s, pos)
	local b1, b2 = s:byte(pos, pos + 1)
	return b1 + b2 * 256
end

local function read_u32_le(s, pos)
	local b1, b2, b3, b4 = s:byte(pos, pos + 3)
	return b1 + b2 * 256 + b3 * 65536 + b4 * 16777216
end

-- Fixed payload byte sizes for non-recursive markers (excluding the 1-byte tag)
local FIXED_SIZES = {
	[0]  = 0,  -- TYPE_NIL
	[1]  = 0,  -- TYPE_BOOL_FALSE
	[2]  = 0,  -- TYPE_BOOL_TRUE
	[3]  = 1,  -- TYPE_UINT8
	[4]  = 1,  -- TYPE_INT8
	[5]  = 2,  -- TYPE_INT16
	[6]  = 4,  -- TYPE_INT32
	[7]  = 4,  -- TYPE_FLOAT32
	[8]  = 8,  -- TYPE_FLOAT64
	[11] = 1,  -- TYPE_STR_REF
	[19] = 8,  -- TYPE_HASH (dmhash_t = uint64)
	[20] = 1,  -- TYPE_HASH_REF
	[21] = 12, -- TYPE_VECTOR3
	[22] = 8,  -- TYPE_VECTOR3_XY
	[23] = 0,  -- TYPE_VECTOR3_ZERO
	[24] = 16, -- TYPE_VECTOR4
	[25] = 16, -- TYPE_QUAT
	[26] = 0,  -- TYPE_QUAT_IDENTITY
	[27] = 64, -- TYPE_MATRIX4
}

local TYPE_STR8         = 9
local TYPE_STR32        = 10
local TYPE_TABLE_DICT8  = 12
local TYPE_TABLE_DICT32 = 13
local TYPE_TABLE_ARR8   = 14
local TYPE_TABLE_ARR16  = 15
local TYPE_TABLE_ARR32  = 16
local TYPE_TABLE_MIXED  = 17
local TYPE_TABLE_END    = 18

local function patch_value(s, pos, end_pos, out)
	assert(pos <= end_pos, "Unexpected end of stream at byte " .. pos)
	local tag = s:byte(pos)

	-- fixint (0x80..0xFF): unchanged
	if tag >= 0x80 then
		out[#out + 1] = s:sub(pos, pos)
		return pos + 1
	end

	-- OLD fixstr (0x20..0x3F) -> NEW fixstr (0x60..0x7F)
	if tag >= 0x20 and tag <= 0x3F then
		local len = tag - 0x20
		out[#out + 1] = string.char(tag + 0x40)
		if len > 0 then
			out[#out + 1] = s:sub(pos + 1, pos + len)
		end
		return pos + 1 + len
	end

	-- Fixed-size primitives & userdata
	local fixed = FIXED_SIZES[tag]
	if fixed ~= nil then
		out[#out + 1] = s:sub(pos, pos + fixed)
		return pos + 1 + fixed
	end

	-- Variable-length strings
	if tag == TYPE_STR8 then
		local len = s:byte(pos + 1)
		out[#out + 1] = s:sub(pos, pos + 1 + len)
		return pos + 2 + len
	elseif tag == TYPE_STR32 then
		local len = read_u32_le(s, pos + 1)
		out[#out + 1] = s:sub(pos, pos + 4 + len)
		return pos + 5 + len
	end

	-- Dictionary tables
	if tag == TYPE_TABLE_DICT8 then
		local count = s:byte(pos + 1)
		out[#out + 1] = s:sub(pos, pos + 1)
		pos = pos + 2
		for _ = 1, count do
			pos = patch_value(s, pos, end_pos, out) -- key
			pos = patch_value(s, pos, end_pos, out) -- value
		end
		return pos
	elseif tag == TYPE_TABLE_DICT32 then
		local count = read_u32_le(s, pos + 1)
		out[#out + 1] = s:sub(pos, pos + 4)
		pos = pos + 5
		for _ = 1, count do
			pos = patch_value(s, pos, end_pos, out) -- key
			pos = patch_value(s, pos, end_pos, out) -- value
		end
		return pos
	end

	-- Array tables
	if tag == TYPE_TABLE_ARR8 then
		local len = s:byte(pos + 1)
		out[#out + 1] = s:sub(pos, pos + 1)
		pos = pos + 2
		for _ = 1, len do
			pos = patch_value(s, pos, end_pos, out)
		end
		return pos
	elseif tag == TYPE_TABLE_ARR16 then
		local len = read_u16_le(s, pos + 1)
		out[#out + 1] = s:sub(pos, pos + 2)
		pos = pos + 3
		for _ = 1, len do
			pos = patch_value(s, pos, end_pos, out)
		end
		return pos
	elseif tag == TYPE_TABLE_ARR32 then
		local len = read_u32_le(s, pos + 1)
		out[#out + 1] = s:sub(pos, pos + 4)
		pos = pos + 5
		for _ = 1, len do
			pos = patch_value(s, pos, end_pos, out)
		end
		return pos
	end

	-- Mixed tables
	if tag == TYPE_TABLE_MIXED then
		local array_len = read_u32_le(s, pos + 1)
		out[#out + 1] = s:sub(pos, pos + 4)
		pos = pos + 5
		for _ = 1, array_len do
			pos = patch_value(s, pos, end_pos, out)
		end
		while pos <= end_pos and s:byte(pos) ~= TYPE_TABLE_END do
			pos = patch_value(s, pos, end_pos, out) -- key
			pos = patch_value(s, pos, end_pos, out) -- value
		end
		assert(s:byte(pos) == TYPE_TABLE_END, "Missing TYPE_TABLE_END at byte " .. pos)
		out[#out + 1] = s:sub(pos, pos)
		return pos + 1
	end

	error(string.format("Unknown tag 0x%02X at byte %d", tag, pos))
end

--- Patches an old binary_pack payload to the new fixstr tag range (0x60..0x7F).
--- If has_sha is true, strips the old 32-byte checksum, patches, and re-signs via binary_pack.
function M.patch_payload(raw, has_sha)
	local end_pos = has_sha and (#raw - 32) or #raw
	local out = {}
	local next_pos = patch_value(raw, 1, end_pos, out)
	assert(next_pos == end_pos + 1, "Trailing bytes left after patch")

	local patched = table.concat(out)

	if has_sha then
		-- Changing the tags invalidates the old SHA-256, so decode and re-encode with a fresh checksum
		local tbl = binary_pack.decode(patched)
		return binary_pack.encode_sha(tbl)
	end

	return patched
end

return M
