#define EXTENSION_NAME binary_pack
#define LIB_NAME "binary_pack"
#define MODULE_NAME "binary_pack"

#include <dmsdk/sdk.h>
#include <dmsdk/dlib/crypt.h>
#include <vector>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <float.h>

#define MAX_NESTING_DEPTH 64
#define MAX_STRING_ALLOC (1024 * 1024 * 10) // 10mb limit per string
#define MAX_TABLE_ELEMENTS 1000000          // max elements per table

#define MAX_DICT_ENTRIES 256
#define HASH_LUT_SIZE 512    // lookup table size for encode indexing
#define LUA_STR_CACHE_MAX 16 // max string keys cached on lua stack

// type markers
// 0x00..0x1f -> standard types
// 0x20..0x3f -> fixstr (length 0..31 embedded in marker)
// 0x80..0xff -> fixint (integers 0..127 embedded in marker)

enum TypeMarker : uint8_t {
	TYPE_NIL = 0,
	TYPE_BOOL_FALSE,
	TYPE_BOOL_TRUE,

	// compact numbers
	TYPE_UINT8,
	TYPE_INT8,
	TYPE_INT16,
	TYPE_INT32,
	TYPE_FLOAT32,
	TYPE_FLOAT64,

	// strings and dedupe
	TYPE_STR8,    // short string less than 255 length
	TYPE_STR32,   // long string
	TYPE_STR_REF, // string ref

	// tables
	TYPE_TABLE_DICT8,  // key/value table (up to 255 keys)
	TYPE_TABLE_DICT32, // large key/value table
	TYPE_TABLE_ARR8,   // index table (up to 255 items)
	TYPE_TABLE_ARR16,  // medium index table (up to 65535 items)
	TYPE_TABLE_ARR32,  // large index table
	TYPE_TABLE_MIXED,  // hybrid array and key/value table
	TYPE_TABLE_END,    // end marker for mixed tables

	// defold userdata and dedupe
	TYPE_HASH,
	TYPE_HASH_REF,      // hash reference for dedupe
	TYPE_VECTOR3,
	TYPE_VECTOR3_XY,    // 2d vector3 where z is 0
	TYPE_VECTOR3_ZERO,  // empty vector3 (0,0,0)
	TYPE_VECTOR4,
	TYPE_QUAT,
	TYPE_QUAT_IDENTITY, // default quat (0,0,0,1)
	TYPE_MATRIX4
};

// persistent buffer to avoid heap allocations across calls
static std::vector<uint8_t> g_persistent_buf;


struct serialiser_state {
	std::vector<uint8_t>& data;

	// string dictionary and lookup table
	const char* str_dict[MAX_DICT_ENTRIES];
	uint16_t str_count = 0;
	uint16_t str_lut[HASH_LUT_SIZE];

	// hash dictionary and lookup table
	dmhash_t hash_dict[MAX_DICT_ENTRIES];
	uint16_t hash_count = 0;
	uint16_t hash_lut[HASH_LUT_SIZE];

	const void* visited[MAX_NESTING_DEPTH + 2];
	int visited_count = 0;
	int depth = 0;

	serialiser_state(std::vector<uint8_t>& buf) : data(buf) {
		// shrink buffer if previous encode was over 256kb
		if (data.capacity() > 262144) {
			std::vector<uint8_t>().swap(data); 
		} else {
			data.clear();
		}
		if (data.capacity() < 4096) {
			data.reserve(4096);
		}
		// reset lookup tables for new encode pass
		memset(str_lut, 0, sizeof(str_lut));
		memset(hash_lut, 0, sizeof(hash_lut));
	}

	// helpers to write data to buffer
	inline void write_u8(uint8_t v) { data.push_back(v); }

	inline void write_tag_u8(uint8_t tag, uint8_t v) {
		size_t s = data.size();
		data.resize(s + 2);
		uint8_t* dst = data.data() + s;
		dst[0] = tag;
		dst[1] = v;
	}

	template <typename T>
	inline void write_tag_val(uint8_t tag, T v) {
		size_t s = data.size();
		data.resize(s + 1 + sizeof(T));
		uint8_t* dst = data.data() + s;
		dst[0] = tag;
		memcpy(dst + 1, &v, sizeof(T));
	}

	inline void write(const void* ptr, size_t size) {
		size_t s = data.size();
		data.resize(s + size);
		memcpy(data.data() + s, ptr, size);
	}

	// check if table pointer was already visited
	inline bool has_visited(const void* ptr) const {
		for (int i = 0; i < visited_count; ++i) {
			if (visited[i] == ptr) return true;
		}
		return false;
	}
};

// encode table string keys with dictionary lookup
static inline bool EncodeStringKey(lua_State* L, int index, serialiser_state& buf) {
	size_t len;
	const char* str = lua_tolstring(L, index, &len);

	// check string length limit
	if (len > MAX_STRING_ALLOC) {
		dmLogError("binary_pack: String length (%zu) exceeds max limit (%d)", len, MAX_STRING_ALLOC);
		return false;
	}

	// only dedupe short to medium keys
	if (len > 1 && len <= 64) {
		uintptr_t p = (uintptr_t)str;
		uint32_t bucket = (uint32_t)(((p >> 4) ^ (p >> 12)) & (HASH_LUT_SIZE - 1));

		// check if string is already in dictionary
		while (buf.str_lut[bucket] != 0) {
			uint8_t id = (uint8_t)(buf.str_lut[bucket] - 1);
			if (buf.str_dict[id] == str) {
				buf.write_tag_u8(TYPE_STR_REF, id);
				return true;
			}
			bucket = (bucket + 1) & (HASH_LUT_SIZE - 1);
		}

		// not found, add to dictionary if space allows
		if (buf.str_count < MAX_DICT_ENTRIES) {
			uint8_t id = (uint8_t)buf.str_count++;
			buf.str_dict[id] = str;
			buf.str_lut[bucket] = (uint16_t)(id + 1);
		}
	}

	// write string header and data based on length
	if (len <= 31) {
		buf.write_u8((uint8_t)(0x20 | len));
	} else if (len <= 255) {
		buf.write_tag_u8(TYPE_STR8, (uint8_t)len);
	} else {
		buf.write_tag_val<uint32_t>(TYPE_STR32, (uint32_t)len);
	}
	buf.write(str, len);
	return true;
}


static bool EncodeValue(lua_State* L, int index, serialiser_state& buf) {
	// get lua type
	int type = lua_type(L, index);

	switch (type) {
		// nil
		case LUA_TNIL:
		buf.write_u8(TYPE_NIL);
		break;

		// boolean
		case LUA_TBOOLEAN:
		buf.write_u8(lua_toboolean(L, index) ? TYPE_BOOL_TRUE : TYPE_BOOL_FALSE);
		break;

		// number
		case LUA_TNUMBER: {
			double v = lua_tonumber(L, index);
			// check if number is an exact 32-bit integer
			if (v >= -2147483648.0 && v <= 2147483647.0) {
				int32_t iv = (int32_t)v;
				if ((double)iv == v) {
					// positive integers
					if (iv >= 0) {
						if (iv <= 127) {
							buf.write_u8((uint8_t)(0x80 | iv));
						} else if (iv <= 255) {
							buf.write_tag_u8(TYPE_UINT8, (uint8_t)iv);
						} else if (iv <= 32767) {
							buf.write_tag_val<int16_t>(TYPE_INT16, (int16_t)iv);
						} else {
							buf.write_tag_val<int32_t>(TYPE_INT32, iv);
						}
						// negative integers
					} else {
						if (iv >= -128) {
							buf.write_tag_u8(TYPE_INT8, (uint8_t)((int8_t)iv));
						} else if (iv >= -32768) {
							buf.write_tag_val<int16_t>(TYPE_INT16, (int16_t)iv);
						} else {
							buf.write_tag_val<int32_t>(TYPE_INT32, iv);
						}
					}
					break;
				}
			}
			// use float32 if within range and no precision is lost, otherwise float64
			if (isfinite(v) && fabs(v) <= FLT_MAX) {
				float f32 = (float)v;
				if ((double)f32 == v) {
					buf.write_tag_val<float>(TYPE_FLOAT32, f32);
					break;
				}
			}
			buf.write_tag_val<double>(TYPE_FLOAT64, v);
			break;
		}

		// string value
		case LUA_TSTRING: {
			size_t len;		
			const char* str = lua_tolstring(L, index, &len);
			if (len > MAX_STRING_ALLOC) {
				dmLogError("binary_pack: String length (%zu) exceeds max limit (%d)", len, MAX_STRING_ALLOC);
				return false;
			}

			// write string type based on length
			if (len <= 31) {
				buf.write_u8((uint8_t)(0x20 | len));
			} else if (len <= 255) {
				buf.write_tag_u8(TYPE_STR8, (uint8_t)len);
			} else {
				buf.write_tag_val<uint32_t>(TYPE_STR32, (uint32_t)len);
			}
			buf.write(str, len);
			break;
		}

		// table
		case LUA_TTABLE: {
			// check nesting depth
			if (buf.depth >= MAX_NESTING_DEPTH) {
				dmLogError("binary_pack: Max table nesting depth (%d) exceeded during encode", MAX_NESTING_DEPTH);
				return false;
			}

			// check for circular references
			const void* ptr = lua_topointer(L, index);
			if (buf.has_visited(ptr)) {
				dmLogError("binary_pack: Cycle detected in table serialisation");
				return false;
			}

			if (!lua_checkstack(L, 5)) {
				dmLogError("binary_pack: Lua stack overflow during encode");
				return false;
			}

			int t = index < 0 ? lua_gettop(L) + index + 1 : index; 
			uint32_t array_len = (uint32_t)lua_objlen(L, t);
			if (array_len > MAX_TABLE_ELEMENTS) {
				dmLogError("binary_pack: Table array length (%u) exceeds max limit (%d)", array_len, MAX_TABLE_ELEMENTS);
				return false;
			}

			buf.visited[buf.visited_count++] = ptr;
			buf.depth++;

			// path 1: pure key/value dictionary table
			if (array_len == 0) {
				size_t header_pos = buf.data.size();
				buf.write_tag_u8(TYPE_TABLE_DICT8, 0);

				uint32_t dict_count = 0;
				lua_pushnil(L);
				while (lua_next(L, t) != 0) {
					if (dict_count >= MAX_TABLE_ELEMENTS) {
						dmLogError("binary_pack: Table dictionary count exceeds max limit (%d)", MAX_TABLE_ELEMENTS);
						lua_pop(L, 2);
						return false;
					}

					// switch header to 32-bit count if keys exceed 255
					if (dict_count == 255) {
						buf.data[header_pos] = TYPE_TABLE_DICT32;
						uint8_t pad[3] = { 0, 0, 0 };
						buf.data.insert(buf.data.begin() + header_pos + 2, pad, pad + 3);
					}

					// encode key
					if (lua_type(L, -2) == LUA_TSTRING) {
						if (!EncodeStringKey(L, -2, buf)) {
							lua_pop(L, 2);
							return false;
						}
					} else {
						if (!EncodeValue(L, -2, buf)) {
							lua_pop(L, 2);
							return false;
						}
					}

					// encode value
					if (!EncodeValue(L, -1, buf)) {
						lua_pop(L, 2);
						return false;
					}
					dict_count++;
					lua_pop(L, 1);
				}

				// write final key count into header
				if (dict_count <= 255) {
					buf.data[header_pos + 1] = (uint8_t)dict_count;
				} else {
					memcpy(&buf.data[header_pos + 1], &dict_count, sizeof(uint32_t));
				}

				// path 2: indexed array table (or mixed table)
			} else {
				size_t header_pos = buf.data.size();
				if (array_len <= 255) {
					buf.write_tag_u8(TYPE_TABLE_ARR8, (uint8_t)array_len);
				} else if (array_len <= 65535) {
					buf.write_tag_val<uint16_t>(TYPE_TABLE_ARR16, (uint16_t)array_len);
				} else {
					buf.write_tag_val<uint32_t>(TYPE_TABLE_ARR32, array_len);
				}

				// write sequential array items
				for (uint32_t i = 1; i <= array_len; ++i) {
					lua_rawgeti(L, t, i);
					if (!EncodeValue(L, -1, buf)) {
						lua_pop(L, 1);
						return false;
					}
					lua_pop(L, 1);
				}

				// check if any extra dictionary keys exist past array_len
				lua_pushinteger(L, (lua_Integer)array_len);
				if (lua_next(L, t) != 0) {
					lua_pop(L, 2);

					uint32_t dict_count = 0;
					lua_pushnil(L);
					while (lua_next(L, t) != 0) {
						int key_type = lua_type(L, -2);
						// skip array indices already written
						if (key_type == LUA_TNUMBER) {
							double k = lua_tonumber(L, -2);
							uint32_t ik = (uint32_t)k;
							if ((double)ik == k && ik >= 1 && ik <= array_len) {
								lua_pop(L, 1);
								continue;
							}
						}

						if (dict_count >= MAX_TABLE_ELEMENTS) {
							dmLogError("binary_pack: Table dictionary count exceeds max limit (%d)", MAX_TABLE_ELEMENTS);
							lua_pop(L, 2);
							return false;
						}

						// upgrade header to mixed table on first extra key
						if (dict_count == 0) {
							uint8_t old_type = buf.data[header_pos];
							buf.data[header_pos] = TYPE_TABLE_MIXED;
							if (old_type == TYPE_TABLE_ARR8) {
								uint8_t pad[3] = { 0, 0, 0 };
								buf.data.insert(buf.data.begin() + header_pos + 2, pad, pad + 3);
							} else if (old_type == TYPE_TABLE_ARR16) {
								uint8_t pad[2] = { 0, 0 };
								buf.data.insert(buf.data.begin() + header_pos + 3, pad, pad + 2);
							}
							memcpy(&buf.data[header_pos + 1], &array_len, sizeof(uint32_t));
						}

						// encode extra key and value
						if (key_type == LUA_TSTRING) {
							if (!EncodeStringKey(L, -2, buf)) {
								lua_pop(L, 2);
								return false;
							}
						} else {
							if (!EncodeValue(L, -2, buf)) {
								lua_pop(L, 2);
								return false;
							}
						}

						if (!EncodeValue(L, -1, buf)) {
							lua_pop(L, 2);
							return false;
						}
						dict_count++;
						lua_pop(L, 1);
					}

					// mark end of mixed table
					if (dict_count > 0) {
						buf.write_u8(TYPE_TABLE_END);
					}
				}
			}

			buf.depth--;
			buf.visited_count--;
			break;
		}

		// defold userdata types
		case LUA_TUSERDATA: {
			// hash
			if (dmScript::IsHash(L, index)) {
				dmhash_t h = dmScript::CheckHash(L, index);
				uint32_t bucket = (uint32_t)((h ^ (h >> 8)) & (HASH_LUT_SIZE - 1));

				// check if hash is in dictionary
				while (buf.hash_lut[bucket] != 0) {
					uint8_t id = (uint8_t)(buf.hash_lut[bucket] - 1);
					if (buf.hash_dict[id] == h) {
						buf.write_tag_u8(TYPE_HASH_REF, id);
						return true;
					}
					bucket = (bucket + 1) & (HASH_LUT_SIZE - 1);
				}

				// add new hash to dictionary
				if (buf.hash_count < MAX_DICT_ENTRIES) {
					uint8_t id = (uint8_t)buf.hash_count++;
					buf.hash_dict[id] = h;
					buf.hash_lut[bucket] = (uint16_t)(id + 1);
				}
				buf.write_tag_val<dmhash_t>(TYPE_HASH, h);
			} 
			// vector3
			else if (dmScript::IsVector3(L, index)) {
				dmVMath::Vector3* v = dmScript::CheckVector3(L, index);
				float x = v->getX(), y = v->getY(), z = v->getZ();
				if (z == 0.0f) {
					if (x == 0.0f && y == 0.0f) {
						buf.write_u8(TYPE_VECTOR3_ZERO);
					} else {
						float f[2] = { x, y };
						buf.write_u8(TYPE_VECTOR3_XY);
						buf.write(f, sizeof(f));
					}
				} else {
					float f[3] = { x, y, z };
					buf.write_u8(TYPE_VECTOR3);
					buf.write(f, sizeof(f));
				}
			} 
			// vector4
			else if (dmScript::IsVector4(L, index)) {
				dmVMath::Vector4* v = dmScript::CheckVector4(L, index);
				float f[4] = { v->getX(), v->getY(), v->getZ(), v->getW() };
				buf.write_u8(TYPE_VECTOR4);
				buf.write(f, sizeof(f));
			} 
			// quaternion
			else if (dmScript::IsQuat(L, index)) {
				dmVMath::Quat* q = dmScript::CheckQuat(L, index);
				float x = q->getX(), y = q->getY(), z = q->getZ(), w = q->getW();
				if (x == 0.0f && y == 0.0f && z == 0.0f && w == 1.0f) {
					buf.write_u8(TYPE_QUAT_IDENTITY);
				} else {
					float f[4] = { x, y, z, w };
					buf.write_u8(TYPE_QUAT);
					buf.write(f, sizeof(f));
				}
			}
			// matrix4
			else if (dmScript::IsMatrix4(L, index)) {
				dmVMath::Matrix4* m = dmScript::CheckMatrix4(L, index);
				float f[16];
				for (int col = 0; col < 4; ++col) {
					dmVMath::Vector4 c = m->getCol(col);
					f[col * 4 + 0] = c.getX();
					f[col * 4 + 1] = c.getY();
					f[col * 4 + 2] = c.getZ();
					f[col * 4 + 3] = c.getW();
				}
				buf.write_u8(TYPE_MATRIX4);
				buf.write(f, sizeof(f));
			} 
			else {
				dmLogError("binary_pack: Unsupported userdata type");
				return false;
			}
			break;
		}

		default:
		dmLogError("binary_pack: Unsupported type %s", lua_typename(L, type));
		return false;
	}
	return true;
}

// state for decode pass
struct decode_context {
	const char* str_ptrs[MAX_DICT_ENTRIES];
	uint8_t str_lens[MAX_DICT_ENTRIES];
	uint16_t str_count = 0;

	// stack cache tracking for repeated string keys
	uint32_t str_cached_mask = 0;
	int str_cache_base = 0;

	dmhash_t hash_dict[MAX_DICT_ENTRIES];
	uint16_t hash_count = 0;
	int depth = 0;
};

// forward declaration
static bool DecodeValue(lua_State* L, const uint8_t*& ptr, const uint8_t* end, decode_context& ctx);

// decode table keys
static inline bool DecodeKey(lua_State* L, const uint8_t*& ptr, const uint8_t* end, decode_context& ctx) {
	if (ptr >= end) return false;
	uint8_t ktype = *ptr;

	// path 1: string reference from dictionary
	if (ktype == TYPE_STR_REF) {
		ptr++;
		if ((size_t)(end - ptr) < 1) return false;
		uint8_t ref_id = *ptr++;
		if (ref_id >= ctx.str_count) {
			dmLogError("binary_pack: Corrupt string reference ID");
			return false;
		}
		// push from lua stack cache if available, otherwise cache it now
		if (ref_id < LUA_STR_CACHE_MAX) {
			uint32_t bit = (1u << ref_id);
			if (ctx.str_cached_mask & bit) {
				lua_pushvalue(L, ctx.str_cache_base + ref_id);
			} else {
				lua_pushlstring(L, ctx.str_ptrs[ref_id], ctx.str_lens[ref_id]);
				lua_pushvalue(L, -1);
				lua_replace(L, ctx.str_cache_base + ref_id);
				ctx.str_cached_mask |= bit;
			}
		} else {
			lua_pushlstring(L, ctx.str_ptrs[ref_id], ctx.str_lens[ref_id]);
		}
		return true;
	}

	// path 2: short fixstr key (0..31 bytes)
	if ((ktype & 0xE0) == 0x20) {
		ptr++;
		uint8_t len = ktype & 0x1F;
		if (len > (size_t)(end - ptr)) return false;
		const char* s = (const char*)ptr;
		lua_pushlstring(L, s, len);
		ptr += len;
		if (len > 1 && ctx.str_count < MAX_DICT_ENTRIES) {
			uint16_t id = ctx.str_count++;
			ctx.str_ptrs[id] = s;
			ctx.str_lens[id] = len;
		}
		return true;
	}

	// path 3: medium string key (32..255 bytes)
	if (ktype == TYPE_STR8) {
		ptr++;
		if ((size_t)(end - ptr) < 1) return false;
		uint8_t len = *ptr++;
		if (len > (size_t)(end - ptr)) return false;
		const char* s = (const char*)ptr;
		lua_pushlstring(L, s, len);
		ptr += len;
		if (len > 1 && len <= 64 && ctx.str_count < MAX_DICT_ENTRIES) {
			uint16_t id = ctx.str_count++;
			ctx.str_ptrs[id] = s;
			ctx.str_lens[id] = len;
		}
		return true;
	}

	// fallback for non-string keys (numbers, hashes, etc.)
	if (!DecodeValue(L, ptr, end, ctx)) return false;
	int key_type = lua_type(L, -1);
	if (key_type == LUA_TNIL) {
		dmLogError("binary_pack: Corrupt data - table key cannot be nil");
		return false;
	}
	if (key_type == LUA_TNUMBER && isnan(lua_tonumber(L, -1))) {
		dmLogError("binary_pack: Corrupt data - table key cannot be NaN");
		return false;
	}
	return true;
}

// decode sequential array items
static inline bool DecodeArrayElements(lua_State* L, uint32_t array_len, const uint8_t*& ptr, const uint8_t* end, decode_context& ctx) {
	for (uint32_t i = 1; i <= array_len; ++i) {
		if (ptr >= end) return false;
		uint8_t etype = *ptr;
		// fast paths for common array primitives
		if (etype & 0x80) {
			ptr++;
			lua_pushnumber(L, (lua_Number)(etype & 0x7F));
		} else if (etype == TYPE_INT16) {
			ptr++;
			if ((size_t)(end - ptr) < sizeof(int16_t)) return false;
			int16_t v;
			memcpy(&v, ptr, sizeof(int16_t));
			ptr += sizeof(int16_t);
			lua_pushnumber(L, (lua_Number)v);
		} else if (etype == TYPE_FLOAT32) {
			ptr++;
			if ((size_t)(end - ptr) < sizeof(float)) return false;
			float v;
			memcpy(&v, ptr, sizeof(float));
			ptr += sizeof(float);
			lua_pushnumber(L, (lua_Number)v);
		} else if (etype == TYPE_BOOL_TRUE || etype == TYPE_BOOL_FALSE) {
			ptr++;
			lua_pushboolean(L, etype == TYPE_BOOL_TRUE);
		} else {
			if (!DecodeValue(L, ptr, end, ctx)) return false;
		}
		lua_rawseti(L, -2, (int)i);
	}
	return true;
}

static bool DecodeValue(lua_State* L, const uint8_t*& ptr, const uint8_t* end, decode_context& ctx) {
	if (ptr >= end) return false;
	uint8_t type = *ptr++;

	// path 1: embedded fixint (0..127)
	if (type & 0x80) {
		lua_pushnumber(L, (lua_Number)(type & 0x7F));
		return true;
	}

	// path 2: embedded fixstr (0..31 bytes)
	if ((type & 0xE0) == 0x20) {
		uint8_t len = type & 0x1F;
		if (len > (size_t)(end - ptr)) return false;
		lua_pushlstring(L, (const char*)ptr, len);
		ptr += len;
		return true;
	}

	switch (type) {
		// nil
		case TYPE_NIL:
		lua_pushnil(L);
		break;

		// booleans
		case TYPE_BOOL_FALSE:
		lua_pushboolean(L, 0);
		break;

		case TYPE_BOOL_TRUE:
		lua_pushboolean(L, 1);
		break;

		// uint8
		case TYPE_UINT8: {
			if ((size_t)(end - ptr) < 1) return false;
			lua_pushnumber(L, (lua_Number)(*ptr++));
			break;
		}

		// int8
		case TYPE_INT8: {
			if ((size_t)(end - ptr) < 1) return false;
			lua_pushnumber(L, (lua_Number)((int8_t)(*ptr++)));
			break;
		}

		// int16
		case TYPE_INT16: {
			if ((size_t)(end - ptr) < sizeof(int16_t)) return false;
			int16_t v;
			memcpy(&v, ptr, sizeof(int16_t));
			ptr += sizeof(int16_t);
			lua_pushnumber(L, (lua_Number)v);
			break;
		}

		// int32
		case TYPE_INT32: {
			if ((size_t)(end - ptr) < sizeof(int32_t)) return false;
			int32_t v;
			memcpy(&v, ptr, sizeof(int32_t));
			ptr += sizeof(int32_t);
			lua_pushnumber(L, (lua_Number)v);
			break;
		}

		// float32
		case TYPE_FLOAT32: {
			if ((size_t)(end - ptr) < sizeof(float)) return false;
			float v;
			memcpy(&v, ptr, sizeof(float));
			ptr += sizeof(float);
			lua_pushnumber(L, (lua_Number)v);
			break;
		}

		// float64
		case TYPE_FLOAT64: {
			if ((size_t)(end - ptr) < sizeof(double)) return false;
			double v;
			memcpy(&v, ptr, sizeof(double));
			ptr += sizeof(double);
			lua_pushnumber(L, v);
			break;
		}

		// short string
		case TYPE_STR8: {
			if ((size_t)(end - ptr) < 1) return false;
			uint8_t len = *ptr++;
			if (len > (size_t)(end - ptr)) return false;
			lua_pushlstring(L, (const char*)ptr, len);
			ptr += len;
			break;
		}

		// long string
		case TYPE_STR32: {
			if ((size_t)(end - ptr) < sizeof(uint32_t)) return false;
			uint32_t len;
			memcpy(&len, ptr, sizeof(uint32_t));
			ptr += sizeof(uint32_t);

			if (len > (size_t)(end - ptr) || len > MAX_STRING_ALLOC) {
				dmLogError("binary_pack: Invalid string length or OOM limit triggered");
				return false;
			}

			lua_pushlstring(L, (const char*)ptr, len);
			ptr += len;
			break;
		}

		// string dictionary ref
		case TYPE_STR_REF: {
			if ((size_t)(end - ptr) < 1) return false;
			uint8_t ref_id = *ptr++;
			if (ref_id >= ctx.str_count) {
				dmLogError("binary_pack: Corrupt string reference ID");
				return false;
			}
			if (ref_id < LUA_STR_CACHE_MAX) {
				uint32_t bit = (1u << ref_id);
				if (ctx.str_cached_mask & bit) {
					lua_pushvalue(L, ctx.str_cache_base + ref_id);
				} else {
					lua_pushlstring(L, ctx.str_ptrs[ref_id], ctx.str_lens[ref_id]);
					lua_pushvalue(L, -1);
					lua_replace(L, ctx.str_cache_base + ref_id);
					ctx.str_cached_mask |= bit;
				}
			} else {
				lua_pushlstring(L, ctx.str_ptrs[ref_id], ctx.str_lens[ref_id]);
			}
			break;
		}

		// key/value dictionary tables
		case TYPE_TABLE_DICT8:
		case TYPE_TABLE_DICT32: {
			if (ctx.depth >= MAX_NESTING_DEPTH) {
				dmLogError("binary_pack: Max table nesting depth (%d) exceeded during decode", MAX_NESTING_DEPTH);
				return false;
			}
			if (!lua_checkstack(L, 5)) {
				dmLogError("binary_pack: Lua stack overflow during decode");
				return false;
			}

			// read key count
			uint32_t dict_count = 0;
			if (type == TYPE_TABLE_DICT8) {
				if ((size_t)(end - ptr) < 1) return false;
				dict_count = *ptr++;
			} else {
				if ((size_t)(end - ptr) < sizeof(uint32_t)) return false;
				memcpy(&dict_count, ptr, sizeof(uint32_t));
				ptr += sizeof(uint32_t);
			}

			// validate count against limit and remaining stream bytes (min 2 bytes per key/value pair)
			if (dict_count > MAX_TABLE_ELEMENTS || (size_t)dict_count * 2 > (size_t)(end - ptr)) {
				return false;
			}

			lua_createtable(L, 0, (int)dict_count);
			ctx.depth++;

			// decode key/value pairs
			for (uint32_t i = 0; i < dict_count; ++i) {
				if (!DecodeKey(L, ptr, end, ctx)) return false;
				if (!DecodeValue(L, ptr, end, ctx)) return false;
				lua_rawset(L, -3);
			}

			ctx.depth--;
			break;
		}

		// indexed array tables
		case TYPE_TABLE_ARR8:
		case TYPE_TABLE_ARR16:
		case TYPE_TABLE_ARR32: {
			if (ctx.depth >= MAX_NESTING_DEPTH) {
				dmLogError("binary_pack: Max table nesting depth (%d) exceeded during decode", MAX_NESTING_DEPTH);
				return false;
			}
			if (!lua_checkstack(L, 5)) {
				dmLogError("binary_pack: Lua stack overflow during decode");
				return false;
			}

			// read array length
			uint32_t array_len = 0;
			if (type == TYPE_TABLE_ARR8) {
				if ((size_t)(end - ptr) < 1) return false;
				array_len = *ptr++;
			} else if (type == TYPE_TABLE_ARR16) {
				if ((size_t)(end - ptr) < sizeof(uint16_t)) return false;
				uint16_t len16;
				memcpy(&len16, ptr, sizeof(uint16_t));
				ptr += sizeof(uint16_t);
				array_len = len16;
			} else {
				if ((size_t)(end - ptr) < sizeof(uint32_t)) return false;
				memcpy(&array_len, ptr, sizeof(uint32_t));
				ptr += sizeof(uint32_t);
			}

			// validate length against limit and remaining stream bytes (min 1 byte per item)
			if (array_len > MAX_TABLE_ELEMENTS || array_len > (size_t)(end - ptr)) {
				return false;
			}

			lua_createtable(L, (int)array_len, 0);
			ctx.depth++;

			if (!DecodeArrayElements(L, array_len, ptr, end, ctx)) return false;

			ctx.depth--;
			break;
		}

		// mixed array + dictionary table
		case TYPE_TABLE_MIXED: {
			if (ctx.depth >= MAX_NESTING_DEPTH) {
				dmLogError("binary_pack: Max table nesting depth (%d) exceeded during decode", MAX_NESTING_DEPTH);
				return false;
			}
			if (!lua_checkstack(L, 5)) {
				dmLogError("binary_pack: Lua stack overflow during decode");
				return false;
			}

			if ((size_t)(end - ptr) < sizeof(uint32_t)) return false;
			uint32_t array_len;
			memcpy(&array_len, ptr, sizeof(uint32_t));
			ptr += sizeof(uint32_t);
			if (array_len > MAX_TABLE_ELEMENTS || array_len > (size_t)(end - ptr)) return false;

			lua_createtable(L, (int)array_len, 4);
			ctx.depth++;

			// decode array part first, then key/value part until end marker
			if (!DecodeArrayElements(L, array_len, ptr, end, ctx)) return false;

			uint32_t dict_count = 0;
			while (ptr < end && *ptr != TYPE_TABLE_END) {
				if (dict_count >= MAX_TABLE_ELEMENTS) return false;
				if (!DecodeKey(L, ptr, end, ctx)) return false;
				if (!DecodeValue(L, ptr, end, ctx)) return false; 
				lua_rawset(L, -3);
				dict_count++;
			}

			ctx.depth--;
			if (ptr < end && *ptr == TYPE_TABLE_END) {
				ptr++; 
			} else {
				return false; 
			}
			break;
		}

		// hash
		case TYPE_HASH: {
			if ((size_t)(end - ptr) < sizeof(dmhash_t)) return false;
			dmhash_t h;
			memcpy(&h, ptr, sizeof(dmhash_t));
			ptr += sizeof(dmhash_t);
			if (ctx.hash_count < MAX_DICT_ENTRIES) {
				ctx.hash_dict[ctx.hash_count++] = h;
			}
			dmScript::PushHash(L, h);
			break;
		}

		// hash ref
		case TYPE_HASH_REF: {
			if ((size_t)(end - ptr) < 1) return false;
			uint8_t ref_id = *ptr++;
			if (ref_id >= ctx.hash_count) {
				dmLogError("binary_pack: Corrupt hash reference ID");
				return false;
			}
			dmScript::PushHash(L, ctx.hash_dict[ref_id]);
			break;
		}

		// vector3
		case TYPE_VECTOR3: {
			if ((size_t)(end - ptr) < sizeof(float) * 3) return false;
			float v[3];
			memcpy(v, ptr, sizeof(float) * 3);
			ptr += sizeof(float) * 3;
			dmScript::PushVector3(L, dmVMath::Vector3(v[0], v[1], v[2]));
			break;
		}

		// 2d vector3 (z = 0)
		case TYPE_VECTOR3_XY: {
			if ((size_t)(end - ptr) < sizeof(float) * 2) return false;
			float v[2];
			memcpy(v, ptr, sizeof(float) * 2);
			ptr += sizeof(float) * 2;
			dmScript::PushVector3(L, dmVMath::Vector3(v[0], v[1], 0.0f));
			break;
		}

		// empty vector3
		case TYPE_VECTOR3_ZERO: {
			dmScript::PushVector3(L, dmVMath::Vector3(0.0f, 0.0f, 0.0f));
			break;
		}

		// vector4
		case TYPE_VECTOR4: {
			if ((size_t)(end - ptr) < sizeof(float) * 4) return false;
			float v[4];
			memcpy(v, ptr, sizeof(float) * 4);
			ptr += sizeof(float) * 4;
			dmScript::PushVector4(L, dmVMath::Vector4(v[0], v[1], v[2], v[3]));
			break;
		}

		// quaternion
		case TYPE_QUAT: {
			if ((size_t)(end - ptr) < sizeof(float) * 4) return false;
			float v[4];
			memcpy(v, ptr, sizeof(float) * 4);
			ptr += sizeof(float) * 4;
			dmScript::PushQuat(L, dmVMath::Quat(v[0], v[1], v[2], v[3]));
			break;
		}

		// default quaternion
		case TYPE_QUAT_IDENTITY: {
			dmScript::PushQuat(L, dmVMath::Quat(0.0f, 0.0f, 0.0f, 1.0f));
			break;
		}

		// matrix4
		case TYPE_MATRIX4: {
			if ((size_t)(end - ptr) < sizeof(float) * 16) return false;
			float f[16];
			memcpy(f, ptr, sizeof(float) * 16);
			ptr += sizeof(float) * 16;
			dmVMath::Matrix4 mat(
				dmVMath::Vector4(f[0],  f[1],  f[2],  f[3]),
				dmVMath::Vector4(f[4],  f[5],  f[6],  f[7]),
				dmVMath::Vector4(f[8],  f[9],  f[10], f[11]),
				dmVMath::Vector4(f[12], f[13], f[14], f[15])
			);
			dmScript::PushMatrix4(L, mat);
			break;
		}

		default:
		return false; 
	}
	return true;
}

// check if first byte is a valid table marker
static inline bool IsTableMarker(uint8_t marker) {
	return marker >= TYPE_TABLE_DICT8 && marker <= TYPE_TABLE_MIXED;
}

static int encode(lua_State* L) {
	// require table at arg 1
	luaL_checktype(L, 1, LUA_TTABLE); 
	serialiser_state buf(g_persistent_buf);

	if (!EncodeValue(L, 1, buf)) {
		return luaL_error(L, "binary_pack: Failed to serialise table");
	}

	lua_pushlstring(L, (const char*)buf.data.data(), buf.data.size());
	return 1;
}

static int decode(lua_State* L) {
	size_t len;
	const char* str = luaL_checklstring(L, 1, &len);

	const uint8_t* ptr = (const uint8_t*)str; 
	const uint8_t* end = ptr + len; 

	// check root marker is a table
	if (len < 2 || !IsTableMarker(*ptr)) {
		return luaL_error(L, "binary_pack: Invalid payload (root value must be a table)");
	}

	// reserve stack slots for string cache
	lua_settop(L, 1);
	luaL_checkstack(L, LUA_STR_CACHE_MAX + 8, "binary_pack: stack overflow");
	decode_context ctx;
	ctx.str_cache_base = 2;
	lua_settop(L, 1 + LUA_STR_CACHE_MAX);

	// decode table
	if (!DecodeValue(L, ptr, end, ctx) || ptr != end) {
		return luaL_error(L, "binary_pack: Failed to deserialise table (corrupt or incomplete)");
	}

	return 1;
}

static int encode_sha(lua_State* L) {
	// require table at arg 1
	luaL_checktype(L, 1, LUA_TTABLE); 
	serialiser_state buf(g_persistent_buf);

	if (!EncodeValue(L, 1, buf)) {
		return luaL_error(L, "binary_pack: Failed to serialise table");
	}

	// append sha256 hash to end of buffer
	uint8_t digest[32]; 
	dmCrypt::HashSha256(buf.data.data(), buf.data.size(), digest);
	buf.write(digest, sizeof(digest));

	lua_pushlstring(L, (const char*)buf.data.data(), buf.data.size());
	return 1;
}

static int decode_sha(lua_State* L) {
	size_t len;
	const char* str = luaL_checklstring(L, 1, &len);

	// check minimum size (2 byte table + 32 byte hash)
	if (len < 34) {
		return luaL_error(L, "binary_pack: Payload is too small to contain a valid table and checksum");
	}

	size_t data_len = len - 32;          
	const uint8_t* ptr = (const uint8_t*)str; 
	const uint8_t* file_hash = ptr + data_len; 

	// check root marker is a table
	if (!IsTableMarker(*ptr)) {
		return luaL_error(L, "binary_pack: Invalid payload (root value must be a table)");
	}

	// calculate and compare sha256 hash
	uint8_t calculated_hash[32];
	dmCrypt::HashSha256(ptr, data_len, calculated_hash);

	if (memcmp(calculated_hash, file_hash, 32) != 0) {
		return luaL_error(L, "binary_pack: DATA CORRUPTED OR TAMPERED WITH! Checksum mismatch.");
	}

	const uint8_t* end = ptr + data_len; 

	// reserve stack slots for string cache
	lua_settop(L, 1);
	luaL_checkstack(L, LUA_STR_CACHE_MAX + 8, "binary_pack: stack overflow");
	decode_context ctx;
	ctx.str_cache_base = 2;
	lua_settop(L, 1 + LUA_STR_CACHE_MAX);

	// decode table
	if (!DecodeValue(L, ptr, end, ctx) || ptr != end) {
		return luaL_error(L, "binary_pack: Failed to deserialise table (corrupt or incomplete)");
	}

	return 1;
}

static const luaL_reg Module_methods[] = {
	{"encode", encode},
	{"decode", decode},
	{"encode_sha", encode_sha},
	{"decode_sha", decode_sha},
	{0, 0}
};

static void LuaInit(lua_State* L) {
	int top = lua_gettop(L);
	luaL_register(L, MODULE_NAME, Module_methods);
	lua_pop(L, 1);
}

dmExtension::Result AppInitialize_binary_pack(dmExtension::AppParams* params) 
{ 
	dmLogInfo("binary_pack: Extension initialized.");
	return dmExtension::RESULT_OK; 
}

dmExtension::Result Initialize_binary_pack(dmExtension::Params* params) 
{
	LuaInit(params->m_L);
	return dmExtension::RESULT_OK; 
}

dmExtension::Result AppFinalize_binary_pack(dmExtension::AppParams* params) 
{ 
	// free persistent buffer on shutdown
	std::vector<uint8_t>().swap(g_persistent_buf);
	return dmExtension::RESULT_OK; 
}

dmExtension::Result Finalize_binary_pack(dmExtension::Params* params) 
{ 
	return dmExtension::RESULT_OK; 
}

DM_DECLARE_EXTENSION(EXTENSION_NAME, LIB_NAME, AppInitialize_binary_pack, AppFinalize_binary_pack, Initialize_binary_pack, 0, 0, Finalize_binary_pack)