#pragma once

#include <endian.h>

#include "utils/cast.hpp"

namespace utils {

inline uint16_t HostToLittleEndian(uint16_t value) { return htole16(value); }
inline uint32_t HostToLittleEndian(uint32_t value) { return htole32(value); }
inline uint64_t HostToLittleEndian(uint64_t value) { return htole64(value); }
inline int16_t HostToLittleEndian(int16_t value) {
  return MemcpyCast<int16_t>(htole16(MemcpyCast<uint16_t>(value)));
}
inline int32_t HostToLittleEndian(int32_t value) {
  return MemcpyCast<int32_t>(htole32(MemcpyCast<uint32_t>(value)));
}
inline int64_t HostToLittleEndian(int64_t value) {
  return MemcpyCast<int64_t>(htole64(MemcpyCast<uint64_t>(value)));
}

inline uint16_t LittleEndianToHost(uint16_t value) { return le16toh(value); }
inline uint32_t LittleEndianToHost(uint32_t value) { return le32toh(value); }
inline uint64_t LittleEndianToHost(uint64_t value) { return le64toh(value); }
inline int16_t LittleEndianToHost(int16_t value) {
  return MemcpyCast<int16_t>(le16toh(MemcpyCast<uint16_t>(value)));
}
inline int32_t LittleEndianToHost(int32_t value) {
  return MemcpyCast<int32_t>(le32toh(MemcpyCast<uint32_t>(value)));
}
inline int64_t LittleEndianToHost(int64_t value) {
  return MemcpyCast<int64_t>(le64toh(MemcpyCast<uint64_t>(value)));
}

inline uint16_t HostToBigEndian(uint16_t value) { return htobe16(value); }
inline uint32_t HostToBigEndian(uint32_t value) { return htobe32(value); }
inline uint64_t HostToBigEndian(uint64_t value) { return htobe64(value); }
inline int16_t HostToBigEndian(int16_t value) {
  return MemcpyCast<int16_t>(htobe16(MemcpyCast<uint16_t>(value)));
}
inline int32_t HostToBigEndian(int32_t value) {
  return MemcpyCast<int32_t>(htobe32(MemcpyCast<uint32_t>(value)));
}
inline int64_t HostToBigEndian(int64_t value) {
  return MemcpyCast<int64_t>(htobe64(MemcpyCast<uint64_t>(value)));
}

inline uint16_t BigEndianToHost(uint16_t value) { return be16toh(value); }
inline uint32_t BigEndianToHost(uint32_t value) { return be32toh(value); }
inline uint64_t BigEndianToHost(uint64_t value) { return be64toh(value); }
inline int16_t BigEndianToHost(int16_t value) {
  return MemcpyCast<int16_t>(be16toh(MemcpyCast<uint16_t>(value)));
}
inline int32_t BigEndianToHost(int32_t value) {
  return MemcpyCast<int32_t>(be32toh(MemcpyCast<uint32_t>(value)));
}
inline int64_t BigEndianToHost(int64_t value) {
  return MemcpyCast<int64_t>(be64toh(MemcpyCast<uint64_t>(value)));
}

}  // namespace utils
