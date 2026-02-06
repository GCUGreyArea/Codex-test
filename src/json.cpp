#include "jsonpath/json.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace jsonpath {
namespace {

class Parser {
 public:
  explicit Parser(std::string_view input) : input_(input), pos_(0) {}

  Json parse() {
    Json value = parse_value();
    skip_ws();
    if (pos_ != input_.size()) {
      throw error("Unexpected trailing characters");
    }
    return value;
  }

 private:
  std::string_view input_;
  size_t pos_;

  void skip_ws() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  }

  char peek() const {
    if (pos_ >= input_.size()) {
      return '\0';
    }
    return input_[pos_];
  }

  char get() {
    if (pos_ >= input_.size()) {
      throw error("Unexpected end of input");
    }
    return input_[pos_++];
  }

  Json parse_value() {
    skip_ws();
    char c = peek();
    if (c == '{') {
      return parse_object();
    }
    if (c == '[') {
      return parse_array();
    }
    if (c == '"') {
      return Json(parse_string('"'));
    }
    if (c == 't') {
      expect("true");
      return Json(true);
    }
    if (c == 'f') {
      expect("false");
      return Json(false);
    }
    if (c == 'n') {
      expect("null");
      return Json(nullptr);
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return Json(parse_number());
    }
    throw error("Invalid JSON value");
  }

  Json parse_object() {
    get();
    skip_ws();
    Json::Object obj;
    if (peek() == '}') {
      get();
      return Json(std::move(obj));
    }
    while (true) {
      skip_ws();
      if (peek() != '"') {
        throw error("Expected string key");
      }
      std::string key = parse_string('"');
      skip_ws();
      if (get() != ':') {
        throw error("Expected ':' after key");
      }
      Json value = parse_value();
      obj[std::move(key)] = std::make_shared<Json>(std::move(value));
      skip_ws();
      char c = get();
      if (c == '}') {
        break;
      }
      if (c != ',') {
        throw error("Expected ',' or '}' in object");
      }
    }
    return Json(std::move(obj));
  }

  Json parse_array() {
    get();
    skip_ws();
    Json::Array arr;
    if (peek() == ']') {
      get();
      return Json(std::move(arr));
    }
    while (true) {
      Json value = parse_value();
      arr.push_back(std::make_shared<Json>(std::move(value)));
      skip_ws();
      char c = get();
      if (c == ']') {
        break;
      }
      if (c != ',') {
        throw error("Expected ',' or ']' in array");
      }
    }
    return Json(std::move(arr));
  }

  void expect(const char* keyword) {
    size_t len = std::strlen(keyword);
    if (input_.substr(pos_, len) != keyword) {
      throw error("Unexpected token");
    }
    pos_ += len;
  }

  uint32_t parse_hex4() {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = get();
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        throw error("Invalid hex escape");
      }
    }
    return value;
  }

  static void append_utf8(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
      out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      throw std::runtime_error("Invalid Unicode codepoint");
    }
  }

  std::string parse_string(char quote) {
    if (get() != quote) {
      throw error("Expected string");
    }
    std::string out;
    while (true) {
      char c = get();
      if (c == quote) {
        break;
      }
      if (c == '\\') {
        char e = get();
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t codepoint = parse_hex4();
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
              if (get() != '\\' || get() != 'u') {
                throw error("Invalid surrogate pair");
              }
              uint32_t low = parse_hex4();
              if (low < 0xDC00 || low > 0xDFFF) {
                throw error("Invalid low surrogate");
              }
              codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            }
            append_utf8(out, codepoint);
            break;
          }
          default:
            throw error("Invalid escape sequence");
        }
      } else {
        if (static_cast<unsigned char>(c) < 0x20) {
          throw error("Control character in string");
        }
        out.push_back(c);
      }
    }
    return out;
  }

  double parse_number() {
    size_t start = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    if (peek() == '0') {
      ++pos_;
    } else {
      if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        throw error("Invalid number");
      }
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++pos_;
      }
    }
    if (peek() == '.') {
      ++pos_;
      if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        throw error("Invalid number");
      }
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++pos_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') {
        ++pos_;
      }
      if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        throw error("Invalid number");
      }
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++pos_;
      }
    }

    std::string num_str(input_.substr(start, pos_ - start));
    char* end_ptr = nullptr;
    errno = 0;
    double value = std::strtod(num_str.c_str(), &end_ptr);
    if (errno != 0 || end_ptr == num_str.c_str()) {
      throw error("Invalid number");
    }
    return value;
  }

  std::runtime_error error(const char* message) const {
    return std::runtime_error(std::string(message) + " at position " + std::to_string(pos_));
  }
};

bool json_equal_impl(const Json& lhs, const Json& rhs) {
  if (lhs.value.index() != rhs.value.index()) {
    return false;
  }
  if (lhs.is_null()) {
    return true;
  }
  if (lhs.is_bool()) {
    return lhs.as_bool() == rhs.as_bool();
  }
  if (lhs.is_number()) {
    return lhs.as_number() == rhs.as_number();
  }
  if (lhs.is_string()) {
    return lhs.as_string() == rhs.as_string();
  }
  if (lhs.is_array()) {
    const auto& a = lhs.as_array();
    const auto& b = rhs.as_array();
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      if (!json_equal_impl(*a[i], *b[i])) {
        return false;
      }
    }
    return true;
  }
  const auto& a = lhs.as_object();
  const auto& b = rhs.as_object();
  if (a.size() != b.size()) {
    return false;
  }
  for (const auto& [key, value] : a) {
    auto it = b.find(key);
    if (it == b.end()) {
      return false;
    }
    if (!json_equal_impl(*value, *it->second)) {
      return false;
    }
  }
  return true;
}

}  // namespace

Json parse_json(std::string_view input) {
  Parser parser(input);
  return parser.parse();
}

bool json_equal(const Json& lhs, const Json& rhs) {
  return json_equal_impl(lhs, rhs);
}

}  // namespace jsonpath
