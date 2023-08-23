// Copyright (C) 2020  Matthew "strager" Glazar
// See end of file for extended copyright information.

#include <cstdio>
#include <cstdlib>
#include <quick-lint-js/cli/arg-parser.h>
#include <quick-lint-js/cli/cli-location.h>
#include <quick-lint-js/container/concat.h>
#include <quick-lint-js/container/hash-map.h>
#include <quick-lint-js/container/linked-vector.h>
#include <quick-lint-js/container/padded-string.h>
#include <quick-lint-js/container/string-view.h>
#include <quick-lint-js/container/vector.h>
#include <quick-lint-js/io/file.h>
#include <quick-lint-js/io/output-stream.h>
#include <quick-lint-js/port/char8.h>
#include <quick-lint-js/port/span.h>
#include <quick-lint-js/port/unreachable.h>
#include <quick-lint-js/port/warning.h>
#include <quick-lint-js/reflection/cxx-parser.h>
#include <quick-lint-js/util/algorithm.h>
#include <quick-lint-js/util/cpp.h>
#include <string_view>

QLJS_WARNING_IGNORE_GCC("-Wshadow=compatible-local")

using namespace std::literals::string_view_literals;

namespace quick_lint_js {
namespace {
void write_file_copyright_begin(Output_Stream& out) {
  out.append_literal(
      u8R"(// Copyright (C) 2020  Matthew "strager" Glazar
// See end of file for extended copyright information.

)"_sv);
}

void write_file_generated_comment(Output_Stream& out) {
  out.append_literal(
      u8R"(// Code generated by tools/generate-trace-sources.cpp. DO NOT EDIT.
// source: src/quick-lint-js/logging/trace-types.h
)"_sv);
}

void write_file_copyright_end(Output_Stream& out) {
  out.append_literal(
      u8R"(
// quick-lint-js finds bugs in JavaScript programs.
// Copyright (C) 2020  Matthew "strager" Glazar
//
// This file is part of quick-lint-js.
//
// quick-lint-js is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// quick-lint-js is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with quick-lint-js.  If not, see <https://www.gnu.org/licenses/>.
)");
}

class CXX_Trace_Types_Parser : public CXX_Parser_Base {
 public:
  struct Parsed_Type_Alias {
    String8_View cxx_name;
    String8_View ctf_name;
    String8_View cxx_type;
  };

  struct Parsed_Enum_Member {
    String8_View cxx_name;
    std::uint64_t value;
  };

  struct Parsed_Enum {
    String8_View cxx_name;
    String8_View ctf_name;
    String8_View underlying_cxx_type;
    Span<Parsed_Enum_Member> members;
  };

  struct Parsed_Struct_Member {
    String8_View cxx_type;
    String8_View cxx_name;
    String8_View ctf_size_name;

    // If true, 'type' is the element type of the array.
    bool type_is_array = false;

    bool type_is_zero_terminated = false;
  };

  struct Parsed_Struct {
    String8_View cxx_name;
    String8_View ctf_name;
    std::optional<std::uint64_t> id;
    Span<Parsed_Struct_Member> members;
  };

  enum class Declaration_Kind {
    _enum,
    _struct,
    type_alias,
  };

  struct Parsed_Declaration {
    Declaration_Kind kind;
    union {
      Parsed_Type_Alias type_alias;
      Parsed_Enum _enum;
      Parsed_Struct _struct;
    };
  };

  using CXX_Parser_Base::CXX_Parser_Base;

  void parse_file() {
    this->skip_preprocessor_directives();
    this->expect_skip(u8"namespace");
    this->expect_skip(u8"quick_lint_js");
    this->expect_skip(CXX_Token_Type::left_curly);

    while (this->peek().type != CXX_Token_Type::right_curly) {
      if (this->peek().type == CXX_Token_Type::identifier &&
          this->peek().identifier == u8"template"_sv) {
        // template <class Foo, class Bar>
        this->skip();
        while (this->peek().type != CXX_Token_Type::greater) {
          this->skip();
        }
        this->skip();
      } else if (this->peek().type == CXX_Token_Type::identifier &&
                 this->peek().identifier == u8"struct"_sv) {
        // struct Trace_Foo { };
        this->parse_struct();
      } else if (this->peek().type == CXX_Token_Type::identifier &&
                 this->peek().identifier == u8"enum"_sv) {
        // enum class Foo : std::uint8_t { };
        this->parse_enum();
      } else if (this->peek().type == CXX_Token_Type::identifier &&
                 this->peek().identifier == u8"using"_sv) {
        // using A = B;
        this->parse_type_alias();
      } else if (this->peek().type == CXX_Token_Type::identifier &&
                 this->peek().identifier == u8"inline"_sv) {
        // inline constexpr int x = 42;
        this->skip();
        this->expect_skip(u8"constexpr"_sv);
        while (this->peek().type != CXX_Token_Type::semicolon) {
          this->skip();
        }
        this->skip();
      } else {
        this->fatal("expected enum or struct");
      }
    }

    this->expect_skip(CXX_Token_Type::right_curly);
  }

  // struct Trace_Foo { };
  void parse_struct() {
    Parsed_Declaration& declaration =
        this->declarations.emplace_back(Parsed_Declaration{
            .kind = Declaration_Kind::_struct,
            ._struct = {},
        });
    Parsed_Struct& s = declaration._struct;

    this->expect_skip(u8"struct"_sv);

    if (this->peek().type == CXX_Token_Type::left_square) {
      // [[qljs::trace_ctf_name("lsp_documents")]]
      this->skip();
      this->expect_skip(CXX_Token_Type::left_square);
      this->expect_skip(u8"qljs"_sv);
      this->expect_skip(CXX_Token_Type::colon_colon);
      this->expect_skip(u8"trace_ctf_name"_sv);
      this->expect_skip(CXX_Token_Type::left_paren);
      this->expect(CXX_Token_Type::string_literal);
      s.ctf_name = this->peek().decoded_string;
      this->skip();
      this->expect_skip(CXX_Token_Type::right_paren);
      this->expect_skip(CXX_Token_Type::right_square);
      this->expect_skip(CXX_Token_Type::right_square);
    }

    this->expect(CXX_Token_Type::identifier);
    s.cxx_name = this->peek().identifier;
    this->skip();

    this->expect_skip(CXX_Token_Type::left_curly);
    Bump_Vector<Parsed_Struct_Member, Monotonic_Allocator> members(
        "members", &this->memory_);
    while (this->peek().type != CXX_Token_Type::right_curly) {
      if (this->peek().type == CXX_Token_Type::identifier &&
          this->peek().identifier == u8"static"_sv) {
        // static constexpr std::uint8_t id = 0x03;
        this->skip();
        this->expect_skip(u8"constexpr"_sv);
        this->expect_skip(u8"std"_sv);
        this->expect_skip(CXX_Token_Type::colon_colon);
        this->expect_skip(u8"uint8_t"_sv);
        this->expect_skip(u8"id"_sv);
        this->expect_skip(CXX_Token_Type::equal);
        this->expect(CXX_Token_Type::number_literal);
        s.id = this->peek().decoded_number;
        this->skip();
        this->expect_skip(CXX_Token_Type::semicolon);
      } else if (this->peek().type == CXX_Token_Type::identifier &&
                 this->peek().identifier == u8"friend"_sv) {
        // friend bool operator==(...) { ... }
        // friend bool operator==(...);
        this->skip();
        while (this->peek().type != CXX_Token_Type::right_paren) {
          this->skip();
        }
        this->skip();
        if (this->peek().type == CXX_Token_Type::semicolon) {
          // friend bool operator==(...);
          this->skip();
        } else {
          // friend bool operator==(...) { ... }
          while (this->peek().type != CXX_Token_Type::right_curly) {
            this->skip();
          }
          this->skip();
        }
      } else {
        // std::uint64_t timestamp;
        // String uri;
        // Span<const Foo> foos;
        // Span<const Foo<String>> foos;
        Parsed_Struct_Member& member = members.emplace_back();

        if (this->peek().type == CXX_Token_Type::left_square) {
          // [[qljs::trace_ctf_size_name("lsp_documents")]]
          // [[qljs::trace_zero_terminated]]
          this->skip();
          this->expect_skip(CXX_Token_Type::left_square);
          this->expect_skip(u8"qljs"_sv);
          this->expect_skip(CXX_Token_Type::colon_colon);
          this->expect(CXX_Token_Type::identifier);
          if (this->peek().identifier == u8"trace_ctf_size_name"_sv) {
            this->skip();
            this->expect_skip(CXX_Token_Type::left_paren);
            this->expect(CXX_Token_Type::string_literal);
            member.ctf_size_name = this->peek().decoded_string;
            this->skip();
            this->expect_skip(CXX_Token_Type::right_paren);
          } else if (this->peek().identifier == u8"trace_zero_terminated"_sv) {
            member.type_is_zero_terminated = true;
            this->skip();
          } else {
            this->fatal("unknown attribute");
          }
          this->expect_skip(CXX_Token_Type::right_square);
          this->expect_skip(CXX_Token_Type::right_square);
        }

        if (this->peek().type == CXX_Token_Type::identifier &&
            this->peek().identifier == u8"Span"_sv) {
          // Span<const Foo> foos;
          member.type_is_array = true;
          this->skip();
          this->expect_skip(CXX_Token_Type::less);
          this->expect_skip(u8"const"_sv);
        }
        if (!member.type_is_array && !member.ctf_size_name.empty()) {
          CLI_Source_Position p =
              this->locator().position(member.ctf_size_name.data());
          std::fprintf(stderr,
                       "%s:%d:%d: error: trace_ctf_size_name is only allowed "
                       "with Span\n",
                       this->file_path(), p.line_number, p.column_number);
          std::exit(1);
        }

        member.cxx_type = this->parse_simple_type_name();

        if (member.type_is_zero_terminated &&
            !(member.cxx_type == u8"string_view"_sv ||
              member.cxx_type == u8"String8_View" ||
              member.cxx_type == u8"String16"_sv)) {
          CLI_Source_Position p =
              this->locator().position(member.cxx_type.data());
          std::fprintf(stderr,
                       "%s:%d:%d: error: trace_zero_terminated is only allowed "
                       "with string types\n",
                       this->file_path(), p.line_number, p.column_number);
          std::exit(1);
        }

        if (this->peek().type == CXX_Token_Type::less) {
          // Foo<String> foo;
          this->skip();
          while (this->peek().type != CXX_Token_Type::greater) {
            this->skip();
          }
          this->skip();
        }

        if (member.type_is_array) {
          this->expect_skip(CXX_Token_Type::greater);
        }

        this->expect(CXX_Token_Type::identifier);
        member.cxx_name = this->peek().identifier;
        this->skip();

        this->expect_skip(CXX_Token_Type::semicolon);
      }
    }
    s.members = members.release_to_span();
    this->expect_skip(CXX_Token_Type::right_curly);
    this->expect_skip(CXX_Token_Type::semicolon);
  }

  // enum class Foo : std::uint8_t { };
  void parse_enum() {
    Parsed_Declaration& declaration =
        this->declarations.emplace_back(Parsed_Declaration{
            .kind = Declaration_Kind::_enum,
            ._enum = {},
        });
    Parsed_Enum& e = declaration._enum;

    this->expect_skip(u8"enum"_sv);
    this->expect_skip(u8"class"_sv);

    if (this->peek().type == CXX_Token_Type::left_square) {
      // [[qljs::trace_ctf_name("lsp_documents")]]
      this->skip();
      this->expect_skip(CXX_Token_Type::left_square);
      this->expect_skip(u8"qljs"_sv);
      this->expect_skip(CXX_Token_Type::colon_colon);
      this->expect_skip(u8"trace_ctf_name"_sv);
      this->expect_skip(CXX_Token_Type::left_paren);
      this->expect(CXX_Token_Type::string_literal);
      e.ctf_name = this->peek().decoded_string;
      this->skip();
      this->expect_skip(CXX_Token_Type::right_paren);
      this->expect_skip(CXX_Token_Type::right_square);
      this->expect_skip(CXX_Token_Type::right_square);
    }

    this->expect(CXX_Token_Type::identifier);
    e.cxx_name = this->peek().identifier;
    this->skip();

    this->expect_skip(CXX_Token_Type::colon);

    this->expect_skip(u8"std"_sv);
    this->expect_skip(CXX_Token_Type::colon_colon);
    this->expect(CXX_Token_Type::identifier);
    e.underlying_cxx_type = this->peek().identifier;
    this->skip();

    this->expect_skip(CXX_Token_Type::left_curly);
    Bump_Vector<Parsed_Enum_Member, Monotonic_Allocator> members(
        "members", &this->memory_);
    while (this->peek().type != CXX_Token_Type::right_curly) {
      // name = 42,
      Parsed_Enum_Member& member = members.emplace_back();

      this->expect(CXX_Token_Type::identifier);
      member.cxx_name = this->peek().identifier;
      this->skip();

      this->expect_skip(CXX_Token_Type::equal);

      this->expect(CXX_Token_Type::number_literal);
      member.value = this->peek().decoded_number;
      this->skip();

      this->expect_skip(CXX_Token_Type::comma);
    }
    e.members = members.release_to_span();
    this->expect_skip(CXX_Token_Type::right_curly);
    this->expect_skip(CXX_Token_Type::semicolon);
  }

  // using A = B;
  void parse_type_alias() {
    Parsed_Declaration& declaration =
        this->declarations.emplace_back(Parsed_Declaration{
            .kind = Declaration_Kind::type_alias,
            .type_alias = {},
        });
    Parsed_Type_Alias& type_alias = declaration.type_alias;

    this->expect_skip(u8"using"_sv);

    this->expect(CXX_Token_Type::identifier);
    type_alias.cxx_name = this->peek().identifier;
    this->skip();

    if (this->peek().type == CXX_Token_Type::left_square) {
      // [[qljs::trace_ctf_name("document_id")]]
      this->skip();
      this->expect_skip(CXX_Token_Type::left_square);
      this->expect_skip(u8"qljs"_sv);
      this->expect_skip(CXX_Token_Type::colon_colon);
      this->expect_skip(u8"trace_ctf_name"_sv);
      this->expect_skip(CXX_Token_Type::left_paren);
      this->expect(CXX_Token_Type::string_literal);
      type_alias.ctf_name = this->peek().decoded_string;
      this->skip();
      this->expect_skip(CXX_Token_Type::right_paren);
      this->expect_skip(CXX_Token_Type::right_square);
      this->expect_skip(CXX_Token_Type::right_square);
    }

    this->expect_skip(CXX_Token_Type::equal);

    type_alias.cxx_type = this->parse_simple_type_name();

    this->expect_skip(CXX_Token_Type::semicolon);
  }

  // std::uint8_t
  // String8_View
  String8_View parse_simple_type_name() {
    if (this->peek().type == CXX_Token_Type::identifier &&
        this->peek().identifier == u8"std"_sv) {
      this->skip();
      this->expect_skip(CXX_Token_Type::colon_colon);
    }
    this->expect(CXX_Token_Type::identifier);
    String8_View type_name = this->peek().identifier;
    this->skip();
    return type_name;
  }

  Monotonic_Allocator memory_{"CXX_Trace_Types_Parser"};
  Bump_Vector<Parsed_Declaration, Monotonic_Allocator> declarations{
      "declarations", &this->memory_};
};

void write_metadata_cpp(CXX_Trace_Types_Parser& types, Output_Stream& out) {
  write_file_generated_comment(out);
  out.append_literal(
      u8R"---(
#include <quick-lint-js/logging/trace-metadata.h>

namespace quick_lint_js {
const Char8 trace_metadata[] =
    u8R"(/* CTF 1.8 */
)---"_sv);

  write_file_copyright_begin(out);

  out.append_literal(
      u8R"(// This file is a Common Trace Format metadata file in the Trace Stream
// Description Language. https://diamon.org/ctf/
//
// This file describes the binary trace files produced by quick-lint-js.

typealias integer { size = 8;  align = 8; signed = false; byte_order = le; } := u8;
typealias integer { size = 16; align = 8; signed = false; byte_order = le; } := u16;
typealias integer { size = 32; align = 8; signed = false; byte_order = le; } := u32;
typealias integer { size = 64; align = 8; signed = false; byte_order = le; } := u64;

typealias string { encoding = utf8; } := utf8_zstring;

// Allows null code points.
typealias struct {
  u64 code_unit_count;
  u16 code_units[code_unit_count];
} := utf16le_string;

typealias struct {
  u64 byte_count;
  u8 bytes[byte_count];
} := utf8_string;

clock {
  name = monotonic_ns_clock;
  freq = 1000000000;
  absolute = false;
};
typealias integer {
  size = 64;
  align = 8;
  signed = false;
  byte_order = le;
  map = clock.monotonic_ns_clock.value;
} := monotonic_ns_timestamp;

trace {
  major = 1;
  minor = 8;
  uuid = "63697571-2d6b-495f-b93e-736a746e696c";
  byte_order = le;
  packet.header := struct {
    u32 magic;
    u8 uuid[16];
  };
};

stream {
  packet.context := struct {
    u64 thread_id;
    u8 compression_scheme;
  };
  event.header := struct {
    monotonic_ns_timestamp timestamp;
    u8 id;
  };
};
)"_sv);

  Hash_Map<String8_View, String8_View> cxx_name_to_ctf_name(&types.memory_);
  cxx_name_to_ctf_name[u8"uint8_t"_sv] = u8"u8"_sv;
  cxx_name_to_ctf_name[u8"uint16_t"_sv] = u8"u16"_sv;
  cxx_name_to_ctf_name[u8"uint32_t"_sv] = u8"u32"_sv;
  cxx_name_to_ctf_name[u8"uint64_t"_sv] = u8"u64"_sv;
  cxx_name_to_ctf_name[u8"String8_View"_sv] = u8"utf8_string"_sv;
  cxx_name_to_ctf_name[u8"String16"_sv] = u8"utf16le_string"_sv;
  // TODO(strager): Remove std::string_view from the C++ code.
  cxx_name_to_ctf_name[u8"string_view"_sv] = u8"utf8_string"_sv;

  auto get_ctf_name = [&](String8_View cxx_name,
                          bool is_zero_terminated = false) {
    QLJS_ASSERT(!cxx_name.empty());
    auto it = cxx_name_to_ctf_name.find(cxx_name);
    if (it == cxx_name_to_ctf_name.end()) {
      CLI_Source_Position p = types.locator().position(cxx_name.data());
      std::fprintf(stderr, "%s:%d:%d: error: unknown type: %.*s\n",
                   types.file_path(), p.line_number, p.column_number,
                   narrow_cast<int>(cxx_name.size()),
                   to_string_view(cxx_name).data());
      std::exit(1);
    }
    String8_View ctf_name = it->second;

    if (is_zero_terminated) {
      if (ctf_name == u8"utf8_string"_sv) {
        return u8"utf8_zstring"_sv;
      }
      if (ctf_name == u8"utf16_string"_sv) {
        return u8"utf16_zstring"_sv;
      }
      CLI_Source_Position p = types.locator().position(cxx_name.data());
      std::fprintf(stderr,
                   "%s:%d:%d: error: cannot process trace_zero_terminated\n",
                   types.file_path(), p.line_number, p.column_number);
      std::exit(1);
    }

    return ctf_name;
  };

  auto write_struct_member =
      [&](const CXX_Trace_Types_Parser::Parsed_Struct_Member& member,
          String8_View indentation) {
        if (member.type_is_array) {
          out.append_copy(indentation);
          out.append_literal(u8"u64 "_sv);
          if (member.ctf_size_name.empty()) {
            out.append_copy(member.cxx_name);
            out.append_literal(u8"_count"_sv);
          } else {
            out.append_copy(member.ctf_size_name);
          }
          out.append_literal(u8";\n"_sv);
          out.append_copy(indentation);
          out.append_copy(get_ctf_name(
              member.cxx_type,
              /*is_zero_terminated=*/member.type_is_zero_terminated));
          out.append_literal(u8" "_sv);
          out.append_copy(member.cxx_name);
          out.append_literal(u8"["_sv);
          if (member.ctf_size_name.empty()) {
            out.append_copy(member.cxx_name);
            out.append_literal(u8"_count"_sv);
          } else {
            out.append_copy(member.ctf_size_name);
          }
          out.append_literal(u8"];\n"_sv);
        } else {
          out.append_copy(indentation);
          out.append_copy(get_ctf_name(
              member.cxx_type,
              /*is_zero_terminated=*/member.type_is_zero_terminated));
          out.append_literal(u8" "_sv);
          out.append_copy(member.cxx_name);
          out.append_literal(u8";\n"_sv);
        }
      };

  for (const CXX_Trace_Types_Parser::Parsed_Declaration& declaration :
       types.declarations) {
    switch (declaration.kind) {
    case CXX_Trace_Types_Parser::Declaration_Kind::_enum: {
      const CXX_Trace_Types_Parser::Parsed_Enum& e = declaration._enum;
      if (e.ctf_name.empty()) {
        continue;
      }
      cxx_name_to_ctf_name[e.cxx_name] = e.ctf_name;

      out.append_literal(u8"\nenum "_sv);
      out.append_copy(e.ctf_name);
      out.append_literal(u8" : "_sv);
      out.append_copy(get_ctf_name(e.underlying_cxx_type));
      out.append_literal(u8" {\n"_sv);
      for (const auto& member : e.members) {
        out.append_literal(u8"  "_sv);
        out.append_copy(member.cxx_name);
        out.append_literal(u8" = "_sv);
        out.append_decimal_integer(member.value);
        out.append_literal(u8",\n"_sv);
      }
      out.append_literal(u8"}\n"_sv);
      break;
    }

    case CXX_Trace_Types_Parser::Declaration_Kind::_struct: {
      const CXX_Trace_Types_Parser::Parsed_Struct& s = declaration._struct;
      if (s.ctf_name.empty()) {
        continue;
      }
      cxx_name_to_ctf_name[s.cxx_name] = s.ctf_name;

      bool is_event = s.id.has_value();
      if (is_event) {
        out.append_literal(u8"\nevent {\n  id = "_sv);
        out.append_decimal_integer(*s.id);
        out.append_literal(u8";\n  name = \""_sv);
        out.append_copy(s.ctf_name);
        out.append_literal(u8"\";\n  fields := struct {\n"_sv);
        for (const CXX_Trace_Types_Parser::Parsed_Struct_Member& member :
             s.members) {
          write_struct_member(member, u8"    "_sv);
        }
        out.append_literal(u8"  };\n};\n"_sv);
      } else {
        out.append_literal(u8"\ntypealias struct {\n"_sv);
        for (const CXX_Trace_Types_Parser::Parsed_Struct_Member& member :
             s.members) {
          write_struct_member(member, u8"  "_sv);
        }
        out.append_literal(u8"} := "_sv);
        out.append_copy(s.ctf_name);
        out.append_literal(u8";\n"_sv);
      }
      break;
    }

    case CXX_Trace_Types_Parser::Declaration_Kind::type_alias: {
      const CXX_Trace_Types_Parser::Parsed_Type_Alias& type_alias =
          declaration.type_alias;
      if (type_alias.ctf_name.empty()) {
        continue;
      }
      cxx_name_to_ctf_name[type_alias.cxx_name] = type_alias.ctf_name;

      out.append_literal(u8"\ntypealias "_sv);
      out.append_copy(get_ctf_name(type_alias.cxx_type));
      out.append_literal(u8" := "_sv);
      out.append_copy(type_alias.ctf_name);
      out.append_literal(u8";\n"_sv);
      break;
    }
    }
  }

  write_file_copyright_end(out);
  out.append_literal(u8")\";\n}\n"_sv);
}
}
}

int main(int argc, char** argv) {
  using namespace quick_lint_js;

  const char* trace_types_h_path = nullptr;
  const char* output_metadata_cpp_path = nullptr;
  Arg_Parser parser(argc, argv);
  QLJS_ARG_PARSER_LOOP(parser) {
    QLJS_ARGUMENT(const char* argument) {
      std::fprintf(stderr, "error: unexpected argument: %s\n", argument);
      std::exit(2);
    }

    QLJS_OPTION(const char* arg_value, "--trace-types-h"sv) {
      trace_types_h_path = arg_value;
    }

    QLJS_OPTION(const char* arg_value, "--output-metadata-cpp"sv) {
      output_metadata_cpp_path = arg_value;
    }

    QLJS_UNRECOGNIZED_OPTION(const char* unrecognized) {
      std::fprintf(stderr, "error: unrecognized option: %s\n", unrecognized);
      std::exit(2);
    }
  }
  if (trace_types_h_path == nullptr) {
    std::fprintf(stderr, "error: missing --trace-types-h\n");
    std::exit(2);
  }
  if (output_metadata_cpp_path == nullptr) {
    std::fprintf(stderr, "error: missing --output-metadata-cpp\n");
    std::exit(2);
  }

  Result<Padded_String, Read_File_IO_Error> trace_types_source =
      read_file(trace_types_h_path);
  if (!trace_types_source.ok()) {
    std::fprintf(stderr, "error: %s\n",
                 trace_types_source.error_to_string().c_str());
    std::exit(1);
  }

  CLI_Locator locator(&*trace_types_source);
  CXX_Trace_Types_Parser cxx_parser(&*trace_types_source, trace_types_h_path,
                                    &locator);
  cxx_parser.parse_file();

  {
    Result<Platform_File, Write_File_IO_Error> output_metadata_cpp =
        open_file_for_writing(output_metadata_cpp_path);
    if (!output_metadata_cpp.ok()) {
      std::fprintf(stderr, "error: %s\n",
                   output_metadata_cpp.error_to_string().c_str());
      std::exit(1);
    }
    File_Output_Stream out(output_metadata_cpp->ref());
    write_metadata_cpp(cxx_parser, out);
    out.flush();
  }

  return 0;
}

// quick-lint-js finds bugs in JavaScript programs.
// Copyright (C) 2020  Matthew "strager" Glazar
//
// This file is part of quick-lint-js.
//
// quick-lint-js is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// quick-lint-js is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with quick-lint-js.  If not, see <https://www.gnu.org/licenses/>.
