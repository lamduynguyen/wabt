#ifndef WABT_DEBUG_LINE_H_
#define WABT_DEBUG_LINE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wabt/base-types.h"
#include "wabt/binary-reader.h"

namespace wabt {

struct DebugLineRow {
  uint32_t address;
  uint32_t file;
  uint32_t line;
  uint32_t column;
  bool end_sequence;
};

struct DebugLineTable {
  std::vector<std::string> files;
  std::vector<DebugLineRow> rows;
};

std::vector<DebugLineTable> DecodeDebugLine(ByteSpan data);
std::optional<Offset> FindCodeSectionStart(ByteSpan module_data);

}  // namespace wabt

#endif  // !WABT_DEBUG_LINE_H_
