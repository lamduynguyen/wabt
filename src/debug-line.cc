#include "wabt/debug-line.h"
#include <sys/types.h>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "wabt/base-types.h"
#include "wabt/binary-reader-nop.h"
#include "wabt/binary-reader.h"
#include "wabt/binary.h"
#include "wabt/leb128.h"
#include "wabt/result.h"

namespace wabt {
namespace {

// read little-endian 32 bit into uint32_t
uint32_t ReadU32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

class CodeSectionFinder : public BinaryReaderNop {
 public:
  std::optional<Offset> code_start;

  Result BeginSection(Index, BinarySection section_type, Offset) override {
    if (section_type == BinarySection::Code) {
      code_start = state->offset;
    }
    return Result::Ok;
  }
};

}  // namespace

std::optional<DebugLineTable> DecodeDebugLineUnit(const uint8_t*& p,
                                                  const uint8_t* end) {
  if (end - p < 4) {
    fprintf(stderr,
            "Error DebugLineTable cannot be read .debug_line section is to "
            "short\n");
    return std::nullopt;
  }
  uint32_t unit_length = ReadU32(p);
  p += 4;
  if (unit_length >= 0xfffffff0) {  // 0xffffffff = 64-bit DWARF
    fprintf(stderr, "debug-line: 64-bit DWARF not supported\n");
    return std::nullopt;
  }
  const uint8_t* unit_end = p + unit_length;
  if (unit_end > end) {
    fprintf(stderr, "Mallformed unit\n");
    return std::nullopt;
  }

  uint16_t version = p[0] | (p[1] << 8);
  p += 2;
  if (version != 4) {
    fprintf(stderr, "debug-line: unsupported version %u (only DWARF4)\n",
            version);
    return std::nullopt;
  }

  uint32_t header_length = ReadU32(p);
  p += 4;
  const uint8_t* program_start = p + header_length;

  if (program_start > unit_end) {
    fprintf(stderr, "Mallformed program start\n");
    return std::nullopt;
  }
  // skipping minimum_instruction_length is for wasm32 always 1.
  p += 1;
  // skipping maximum_instruction_length is for wasm32 always 1.
  p += 1;
  uint8_t default_is_stmt = *p++;
  int8_t line_base = static_cast<int8_t>(*p++);
  uint8_t line_range = *p++;
  uint8_t opcode_base = *p++;
  const uint8_t* std_opcode_lengths = p;
  p += opcode_base - 1;

  // skip include directories
  while (*p != 0) {
    while (*p++ != 0) {
    }
  }
  p++;

  DebugLineTable table;
  // index 0 is not used dwarf starts at 1
  table.files.push_back("");
  while (*p != 0) {
    std::string name(reinterpret_cast<const char*>(p));
    p += name.size() + 1;
    uint32_t dir_index, mtime, size;
    p += ReadU32Leb128(p, unit_end, &dir_index);
    p += ReadU32Leb128(p, unit_end, &mtime);
    p += ReadU32Leb128(p, unit_end, &size);
    table.files.push_back(std::move(name));
  }
  p = program_start;
  uint32_t address = 0, file = 1, line = 1, column = 0;
  bool is_stmt = default_is_stmt;
  while (p < unit_end) {
    uint8_t opcode = *p++;
    if (opcode >= opcode_base) {  // special opcode
      uint8_t adjusted = opcode - opcode_base;
      address += adjusted / line_range;
      line += line_base + (adjusted % line_range);
      table.rows.push_back({address, file, line, column, false});
    } else if (opcode == 0) {  // extended opcode
      uint32_t ext_len;
      p += ReadU32Leb128(p, unit_end, &ext_len);
      const uint8_t* ext_start = p;
      uint8_t sub_opcode = *p;
      if (sub_opcode == 1) {  // DW_LNE_end_sequence
        table.rows.push_back({address, file, line, column, true});
        address = 0;
        file = 1;
        line = 1;
        column = 0;
        is_stmt = default_is_stmt;
      } else if (sub_opcode == 2) {  // DW_LNE_set_address
        address = ReadU32(ext_start + 1);
      }
      p = ext_start + ext_len;
    } else {  // standart opcode
      switch (opcode) {
        case 1: {  // DW_LNS_copy
          table.rows.push_back({address, file, line, column, false});
          break;
        }
        case 2: {  // DW_LNS_advance_pc
          uint32_t adv;
          p += ReadU32Leb128(p, unit_end, &adv);
          address += adv;
          break;
        }
        case 3: {  // DW_LNS_advance_line
          uint32_t adv;
          p += ReadS32Leb128(p, unit_end, &adv);
          line += static_cast<int32_t>(adv);
          break;
        }
        case 4: {  // DW_LNS_set_file
          uint32_t f;
          p += ReadU32Leb128(p, unit_end, &f);
          file = f;
          break;
        }
        case 5: {  // DW_LNS_set_column
          uint32_t c;
          p += ReadU32Leb128(p, unit_end, &c);
          column = c;
          break;
        }
        case 6: {  // DW_LNS_negate_stmt
          is_stmt = !is_stmt;
          break;
        }
        case 7: {  // DW_LNS_set_basic_block
          // currently basic_block is not used therfore we just skip it.
          // basic_block = true;
          break;
        }
        case 8: {  // DW_LNS_const_add_pc
          uint8_t adjusted = 255 - opcode_base;
          address += adjusted / line_range;
          break;
        }
        case 9: {  // DW_LNS_fixed_advance_pc
          uint16_t operand = p[0] | (p[1] << 8);
          p += 2;
          address += operand;
          break;
        }

        default: {
          for (int i = 0; i < std_opcode_lengths[opcode - 1]; i++) {
            uint32_t discard;
            p += ReadU32Leb128(p, unit_end, &discard);
          }
        }
      }
    }
  }

  p = unit_end;
  return table;
}

std::vector<DebugLineTable> DecodeDebugLine(ByteSpan data) {
  const uint8_t* p = data.data();
  const uint8_t* end = p + data.size();
  std::vector<DebugLineTable> tables;
  while (p < end) {
    auto table = DecodeDebugLineUnit(p, end);
    if (!table)
      break;
    tables.push_back(std::move(*table));
  }
  return tables;
}

std::optional<Offset> FindCodeSectionStart(ByteSpan module_data) {
  CodeSectionFinder finder;
  // Best-effort parse: code_start is valid once the code section header has
  // been reached, even if reading the rest of the module fails.
  (void)ReadBinary(module_data, &finder, ReadBinaryOptions());
  return finder.code_start;
}

}  // namespace wabt
