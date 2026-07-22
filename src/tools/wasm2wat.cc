/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "wabt/apply-names.h"
#include "wabt/base-types.h"
#include "wabt/binary-reader-ir.h"
#include "wabt/binary-reader.h"
#include "wabt/debug-line.h"
#include "wabt/error-formatter.h"
#include "wabt/feature.h"
#include "wabt/generate-names.h"
#include "wabt/ir.h"
#include "wabt/option-parser.h"
#include "wabt/result.h"
#include "wabt/stream.h"
#include "wabt/validator.h"
#include "wabt/wast-lexer.h"
#include "wabt/wat-writer.h"

using namespace wabt;

static std::string s_infile;
static std::string s_outfile;
static Features s_features;
static bool s_generate_names;
static bool s_fold_exprs;
static bool s_inline_import;
static bool s_inline_export;
static bool s_read_debug_names = true;
static bool s_fail_on_custom_section_error = true;
static std::unique_ptr<FileStream> s_log_stream;
static bool s_validate = true;
static bool s_annotate_debug_line;

static const char s_description[] =
    R"(  Read a file in the WebAssembly binary format, and convert it to
  the WebAssembly text format.

examples:
  # parse binary file test.wasm and write text file test.wast
  $ wasm2wat test.wasm -o test.wat

  # parse test.wasm, write test.wat, but ignore the debug names, if any
  $ wasm2wat test.wasm --no-debug-names -o test.wat
)";

static void ParseOptions(int argc, char** argv) {
  OptionParser parser("wasm2wat", s_description);

  parser.AddOption('v', "verbose", "Use multiple times for more info",
                   []() { s_log_stream = FileStream::CreateStderr(); });
  parser.AddOption(
      'o', "output", "FILENAME",
      "Output file for the generated wast file, by default use stdout",
      [](const char* argument) {
        s_outfile = argument;
        ConvertBackslashToSlash(&s_outfile);
      });
  parser.AddOption('f', "fold-exprs", "Write folded expressions where possible",
                   []() { s_fold_exprs = true; });
  s_features.AddOptions(&parser);
  parser.AddOption("inline-exports", "Write all exports inline",
                   []() { s_inline_export = true; });
  parser.AddOption("inline-imports", "Write all imports inline",
                   []() { s_inline_import = true; });
  parser.AddOption("no-debug-names", "Ignore debug names in the binary file",
                   []() { s_read_debug_names = false; });
  parser.AddOption("ignore-custom-section-errors",
                   "Ignore errors in custom sections",
                   []() { s_fail_on_custom_section_error = false; });
  parser.AddOption(
      "generate-names",
      "Give auto-generated names to non-named functions, types, etc.",
      []() { s_generate_names = true; });
  parser.AddOption("no-check", "Don't check for invalid modules",
                   []() { s_validate = false; });
  parser.AddOption("debug-line-comments",
                   "Annotate output with source locations from .debug_line",
                   []() { s_annotate_debug_line = true; });
  parser.AddArgument("filename", OptionParser::ArgumentCount::One,
                     [](const char* argument) {
                       s_infile = argument;
                       ConvertBackslashToSlash(&s_infile);
                     });
  parser.Parse(argc, argv);
}

int ProgramMain(int argc, char** argv) {
  Result result;

  InitStdio();
  ParseOptions(argc, argv);

  std::vector<uint8_t> file_data;
  result = ReadFile(s_infile.c_str(), &file_data);
  struct LineEntry {
    Offset address;
    uint32_t line;
    uint32_t column;
    std::string file;
    bool end_sequence;  // gap marker: one byte past a code sequence's end
  };
  std::vector<LineEntry> line_ranges;
  if (Succeeded(result)) {
    Errors errors;
    Module module;
    const bool kStopOnFirstError = true;
    ReadBinaryOptions options(s_features, s_log_stream.get(),
                              s_read_debug_names, kStopOnFirstError,
                              s_fail_on_custom_section_error);
    result =
        ReadBinaryIr(s_infile.c_str(), file_data, options, &errors, &module);
    if (Succeeded(result)) {
      if (Succeeded(result) && s_annotate_debug_line) {
        auto code_start =
            FindCodeSectionStart(ByteSpan(file_data.data(), file_data.size()));
        for (const Custom& custom : module.customs) {
          if (custom.name != ".debug_line" || !code_start)
            continue;
          for (const auto& table : DecodeDebugLine(
                   ByteSpan(custom.data.data(), custom.data.size()))) {
            for (const auto& row : table.rows) {
              if (row.end_sequence) {
                line_ranges.push_back(
                    {row.address + *code_start, 0, 0, "", true});
                continue;
              }
              std::string name =
                  row.file < table.files.size() ? table.files[row.file] : "";
              line_ranges.push_back({row.address + *code_start, row.line,
                                     row.column, std::move(name), false});
            }
          }
        }
        std::sort(
            line_ranges.begin(), line_ranges.end(),
            [](const auto& a, const auto& b) { return a.address < b.address; });
      }
      if (Succeeded(result) && s_validate) {
        ValidateOptions options(s_features);
        result = ValidateModule(&module, &errors, options);
      }

      if (s_generate_names) {
        result = GenerateNames(&module);
      }

      if (Succeeded(result)) {
        /* TODO(binji): This shouldn't fail; if a name can't be applied
         * (because the index is invalid, say) it should just be skipped. */
        Result dummy_result = ApplyNames(&module);
        WABT_USE(dummy_result);
      }

      if (Succeeded(result)) {
        WriteWatOptions wat_options(s_features);
        wat_options.fold_exprs = s_fold_exprs;
        wat_options.inline_import = s_inline_import;
        wat_options.inline_export = s_inline_export;
        wat_options.location_comment =
            [&](Offset offset) -> std::optional<std::string> {
          if (offset == 0 || line_ranges.empty())
            return std::nullopt;
          auto it = std::upper_bound(
              line_ranges.begin(), line_ranges.end(), offset - 1,
              [](Offset value, const auto& e) { return value < e.address; });
          if (it == line_ranges.begin())
            return std::nullopt;
          --it;
          if (it->end_sequence)  // offset falls in a gap between code ranges
            return std::nullopt;
          std::string comment = it->file + ":" + std::to_string(it->line);
          if (it->column != 0) {
            comment += ":" + std::to_string(it->column);  // main.cc:31:7
          }
          return comment;
        };
        FileStream stream(!s_outfile.empty() ? FileStream(s_outfile)
                                             : FileStream(stdout));
        result = WriteWat(&stream, &module, wat_options);
      }
    }
    FormatErrorsToFile(errors, Location::Type::Binary);
  }
  return result != Result::Ok;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
