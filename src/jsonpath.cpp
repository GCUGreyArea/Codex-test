#include "jsonpath/jsonpath.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace jsonpath {
namespace {

constexpr int64_t kMaxIndex = 9007199254740991LL;  // 2^53 - 1

struct Slice {
  std::optional<int64_t> start;
  std::optional<int64_t> end;
  std::optional<int64_t> step;
};

struct Expr;

struct Selector {
  struct Name { std::string value; };
  struct Wildcard {};
  struct Index { int64_t value; };
  struct SliceSel { Slice value; };
  struct Filter { std::unique_ptr<Expr> expr; };

  std::variant<Name, Wildcard, Index, SliceSel, Filter> node;
};

struct Segment {
  bool descendant = false;
  std::vector<Selector> selectors;
};

struct Query {
  bool absolute = true;
  bool singular = true;
  std::vector<Segment> segments;
};

struct Literal {
  Json value;
};

struct FunctionExpr;

struct Comparable {
  std::variant<Literal, Query, std::unique_ptr<FunctionExpr>> node;
};

enum class CompareOp {
  Eq,
  Ne,
  Lt,
  Lte,
  Gt,
  Gte,
};

struct TestItem {
  std::variant<Query, std::unique_ptr<FunctionExpr>> node;
};

struct Expr {
  struct Or { std::unique_ptr<Expr> left; std::unique_ptr<Expr> right; };
  struct And { std::unique_ptr<Expr> left; std::unique_ptr<Expr> right; };
  struct Not { std::unique_ptr<Expr> expr; };
  struct Comparison { Comparable left; CompareOp op; Comparable right; };
  struct Test { TestItem item; };

  std::variant<Or, And, Not, Comparison, Test> node;
};

enum class FnReturn { Value, Logical };

enum class ParamType { Value, Nodes, Logical };

struct FunctionExpr {
  std::string name;
  FnReturn ret;
  std::vector<ParamType> params;
  std::vector<std::variant<Literal, Query, std::unique_ptr<FunctionExpr>, std::unique_ptr<Expr>>> args;
};

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class JsonPathParser {
 public:
  explicit JsonPathParser(std::string_view input) : input_(input) {}

  Query parse_query(bool absolute_required) {
    skip_ws();
    char c = peek();
    if (absolute_required && c != '$') {
      throw error("JSONPath must start with '$'");
    }
    if (c != '$' && c != '@') {
      throw error("Expected '$' or '@'");
    }
    bool absolute = (c == '$');
    get();
    Query query;
    query.absolute = absolute;
    parse_segments(query);
    return query;
  }

  void ensure_end() {
    skip_ws();
    if (pos_ != input_.size()) {
      throw error("Unexpected trailing characters");
    }
  }

 private:
  std::string_view input_;
  size_t pos_ = 0;

  char peek() const {
    if (pos_ >= input_.size()) {
      return '\0';
    }
    return input_[pos_];
  }

  char peek_next() const {
    if (pos_ + 1 >= input_.size()) {
      return '\0';
    }
    return input_[pos_ + 1];
  }

  char get() {
    if (pos_ >= input_.size()) {
      throw error("Unexpected end of input");
    }
    return input_[pos_++];
  }

  void skip_ws() {
    while (pos_ < input_.size()) {
      char c = input_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume(char c) {
    skip_ws();
    if (peek() == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool consume_str(const char* s) {
    skip_ws();
    size_t len = std::strlen(s);
    if (input_.substr(pos_, len) == s) {
      pos_ += len;
      return true;
    }
    return false;
  }

  ParseError error(const char* message) const {
    return ParseError(std::string(message) + " at position " + std::to_string(pos_));
  }

  void parse_segments(Query& query) {
    while (true) {
      skip_ws();
      char c = peek();
      if (c == '.') {
        if (peek_next() == '.') {
          parse_descendant_segment(query);
        } else {
          parse_dot_segment(query);
        }
        continue;
      }
      if (c == '[') {
        parse_bracket_segment(query, false);
        continue;
      }
      break;
    }
  }

  void parse_dot_segment(Query& query) {
    get();
    skip_ws();
    Segment segment;
    segment.descendant = false;
    if (peek() == '*') {
      get();
      query.singular = false;
      segment.selectors.push_back(Selector{Selector::Wildcard{}});
      query.segments.push_back(std::move(segment));
      return;
    }
    std::string name = parse_member_name_shorthand();
    segment.selectors.push_back(Selector{Selector::Name{std::move(name)}});
    query.segments.push_back(std::move(segment));
  }

  void parse_descendant_segment(Query& query) {
    get();
    get();
    query.singular = false;
    Segment segment;
    segment.descendant = true;
    skip_ws();
    if (peek() == '[') {
      parse_bracket_segment(query, true);
      return;
    }
    if (peek() == '*') {
      get();
      segment.selectors.push_back(Selector{Selector::Wildcard{}});
      query.segments.push_back(std::move(segment));
      return;
    }
    std::string name = parse_member_name_shorthand();
    segment.selectors.push_back(Selector{Selector::Name{std::move(name)}});
    query.segments.push_back(std::move(segment));
  }

  void parse_bracket_segment(Query& query, bool descendant) {
    Segment segment;
    segment.descendant = descendant;
    parse_bracketed_selection(query, segment);
    query.segments.push_back(std::move(segment));
  }

  void parse_bracketed_selection(Query& query, Segment& segment) {
    if (!consume('[')) {
      throw error("Expected '['");
    }
    skip_ws();
    bool first = true;
    while (true) {
      if (!first) {
        if (!consume(',')) {
          break;
        }
        skip_ws();
      }
      first = false;
      if (peek() == '?') {
        get();
        auto expr = parse_logical_expr();
        query.singular = false;
        segment.selectors.push_back(Selector{Selector::Filter{std::move(expr)}});
      } else if (peek() == '*') {
        get();
        query.singular = false;
        segment.selectors.push_back(Selector{Selector::Wildcard{}});
      } else if (peek() == '\'' || peek() == '"') {
        std::string name = parse_string_literal();
        segment.selectors.push_back(Selector{Selector::Name{std::move(name)}});
      } else if (peek() == ':' || peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) {
        Selector sel = parse_index_or_slice();
        if (!std::holds_alternative<Selector::Index>(sel.node)) {
          query.singular = false;
        }
        segment.selectors.push_back(std::move(sel));
      } else {
        throw error("Invalid selector in bracketed selection");
      }
      skip_ws();
      if (peek() == ']') {
        break;
      }
    }
    if (!consume(']')) {
      throw error("Expected ']'");
    }

    if (segment.selectors.size() != 1) {
      query.singular = false;
    } else {
      const auto& sel = segment.selectors.front();
      if (!std::holds_alternative<Selector::Name>(sel.node) &&
          !std::holds_alternative<Selector::Index>(sel.node)) {
        query.singular = false;
      }
    }
  }

  std::string parse_member_name_shorthand() {
    skip_ws();
    char c = peek();
    if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80)) {
      throw error("Invalid member name shorthand");
    }
    std::string out;
    out.push_back(get());
    while (true) {
      char n = peek();
      if (std::isalnum(static_cast<unsigned char>(n)) || n == '_' || static_cast<unsigned char>(n) >= 0x80) {
        out.push_back(get());
      } else {
        break;
      }
    }
    return out;
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
      throw ParseError("Invalid Unicode codepoint");
    }
  }

  std::string parse_string_literal() {
    char quote = get();
    std::string out;
    while (true) {
      char c = get();
      if (c == quote) {
        break;
      }
      if (c == '\\') {
        char e = get();
        switch (e) {
          case '\'': out.push_back('\''); break;
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

  int64_t parse_int() {
    skip_ws();
    size_t start = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    if (!std::isdigit(static_cast<unsigned char>(peek()))) {
      throw error("Invalid integer");
    }
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
      ++pos_;
    }
    std::string digits(input_.substr(start, pos_ - start));
    long long value = 0;
    try {
      value = std::stoll(digits);
    } catch (...) {
      throw error("Invalid integer");
    }
    if (value > kMaxIndex || value < -kMaxIndex) {
      throw error("Integer out of range");
    }
    return static_cast<int64_t>(value);
  }

  Selector parse_index_or_slice() {
    skip_ws();
    std::optional<int64_t> start;
    std::optional<int64_t> end;
    std::optional<int64_t> step;
    bool has_start = false;
    if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) {
      start = parse_int();
      has_start = true;
    }
    skip_ws();
    if (peek() == ':') {
      get();
      skip_ws();
      if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) {
        end = parse_int();
      }
      skip_ws();
      if (peek() == ':') {
        get();
        skip_ws();
        if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) {
          step = parse_int();
          if (step == 0) {
            throw error("Slice step cannot be zero");
          }
        }
      }
      Slice slice{start, end, step};
      return Selector{Selector::SliceSel{slice}};
    }
    if (!has_start) {
      throw error("Expected index or slice");
    }
    return Selector{Selector::Index{*start}};
  }

  Literal parse_literal() {
    skip_ws();
    char c = peek();
    if (c == '\'' || c == '"') {
      return Literal{Json(parse_string_literal())};
    }
    if (c == 't') {
      if (consume_str("true")) {
        return Literal{Json(true)};
      }
    }
    if (c == 'f') {
      if (consume_str("false")) {
        return Literal{Json(false)};
      }
    }
    if (c == 'n') {
      if (consume_str("null")) {
        return Literal{Json(nullptr)};
      }
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return Literal{Json(parse_number_literal())};
    }
    throw error("Invalid literal");
  }

  double parse_number_literal() {
    skip_ws();
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
    double value = std::strtod(num_str.c_str(), &end_ptr);
    if (end_ptr == num_str.c_str()) {
      throw error("Invalid number");
    }
    return value;
  }

  Query parse_filter_query() {
    skip_ws();
    char c = peek();
    if (c != '$' && c != '@') {
      throw error("Expected filter query");
    }
    Query query = parse_query(false);
    return query;
  }

  std::unique_ptr<FunctionExpr> parse_function_expr() {
    skip_ws();
    std::string name = parse_member_name_shorthand();
    skip_ws();
    if (!consume('(')) {
      throw error("Expected '(' after function name");
    }

    auto func = std::make_unique<FunctionExpr>();
    func->name = name;
    if (name == "length") {
      func->ret = FnReturn::Value;
      func->params = {ParamType::Value};
    } else if (name == "count") {
      func->ret = FnReturn::Value;
      func->params = {ParamType::Nodes};
    } else if (name == "match") {
      func->ret = FnReturn::Logical;
      func->params = {ParamType::Value, ParamType::Value};
    } else if (name == "search") {
      func->ret = FnReturn::Logical;
      func->params = {ParamType::Value, ParamType::Value};
    } else if (name == "value") {
      func->ret = FnReturn::Value;
      func->params = {ParamType::Nodes};
    } else {
      throw error("Unknown function");
    }

    skip_ws();
    if (peek() == ')') {
      get();
      if (!func->params.empty()) {
        throw error("Function missing arguments");
      }
      return func;
    }

    for (size_t i = 0; i < func->params.size(); ++i) {
      if (i > 0) {
        if (!consume(',')) {
          throw error("Expected ',' between arguments");
        }
      }
      func->args.push_back(parse_argument_for(func->params[i]));
    }

    skip_ws();
    if (!consume(')')) {
      throw error("Expected ')' after arguments");
    }

    if (func->args.size() != func->params.size()) {
      throw error("Incorrect argument count");
    }

    return func;
  }

  std::variant<Literal, Query, std::unique_ptr<FunctionExpr>, std::unique_ptr<Expr>>
  parse_argument_for(ParamType param) {
    skip_ws();
    if (param == ParamType::Nodes) {
      Query query = parse_filter_query();
      return query;
    }
    if (param == ParamType::Value) {
      char c = peek();
      if (c == '\'' || c == '"' || c == '-' || std::isdigit(static_cast<unsigned char>(c)) || c == 't' || c == 'f' || c == 'n') {
        return parse_literal();
      }
      if (c == '$' || c == '@') {
        Query query = parse_filter_query();
        if (!query.singular) {
          throw error("Expected singular query for value argument");
        }
        return query;
      }
      if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
        auto func = parse_function_expr();
        if (func->ret != FnReturn::Value) {
          throw error("Expected value-returning function");
        }
        return func;
      }
      throw error("Invalid value argument");
    }

    // Logical argument
    if (param == ParamType::Logical) {
      auto expr = parse_logical_expr();
      return expr;
    }

    throw error("Invalid function parameter");
  }

  Comparable parse_comparable() {
    skip_ws();
    char c = peek();
    if (c == '\'' || c == '"' || c == '-' || std::isdigit(static_cast<unsigned char>(c)) || c == 't' || c == 'f' || c == 'n') {
      return Comparable{parse_literal()};
    }
    if (c == '$' || c == '@') {
      Query query = parse_filter_query();
      if (!query.singular) {
        throw error("Comparable requires singular query");
      }
      return Comparable{std::move(query)};
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
      auto func = parse_function_expr();
      if (func->ret != FnReturn::Value) {
        throw error("Comparable requires value-returning function");
      }
      return Comparable{std::move(func)};
    }
    throw error("Invalid comparable");
  }

  std::unique_ptr<Expr> parse_logical_expr() {
    auto left = parse_logical_and();
    while (true) {
      if (consume_str("||")) {
        auto right = parse_logical_and();
        auto expr = std::make_unique<Expr>();
        expr->node = Expr::Or{std::move(left), std::move(right)};
        left = std::move(expr);
        continue;
      }
      break;
    }
    return left;
  }

  std::unique_ptr<Expr> parse_logical_and() {
    auto left = parse_basic_expr();
    while (true) {
      if (consume_str("&&")) {
        auto right = parse_basic_expr();
        auto expr = std::make_unique<Expr>();
        expr->node = Expr::And{std::move(left), std::move(right)};
        left = std::move(expr);
        continue;
      }
      break;
    }
    return left;
  }

  std::unique_ptr<Expr> parse_basic_expr() {
    skip_ws();
    bool negate = false;
    if (peek() == '!') {
      get();
      negate = true;
      skip_ws();
      if (peek() == '(') {
        get();
        auto inner = parse_logical_expr();
        if (!consume(')')) {
          throw error("Expected ')' after expression");
        }
        auto expr = std::make_unique<Expr>();
        expr->node = Expr::Not{std::move(inner)};
        return expr;
      }
    }

    if (peek() == '(') {
      get();
      auto inner = parse_logical_expr();
      if (!consume(')')) {
        throw error("Expected ')' after expression");
      }
      return inner;
    }

    auto expr = parse_test_or_comparison();
    if (negate) {
      if (!std::holds_alternative<Expr::Test>(expr->node)) {
        throw error("'!' can only be used with test expressions or parenthesized expressions");
      }
      auto wrapper = std::make_unique<Expr>();
      wrapper->node = Expr::Not{std::move(expr)};
      return wrapper;
    }
    return expr;
  }

  std::unique_ptr<Expr> parse_test_or_comparison() {
    skip_ws();
    char c = peek();
    if (c == '\'' || c == '"' || c == '-' || std::isdigit(static_cast<unsigned char>(c)) || c == 't' || c == 'f' || c == 'n') {
      Comparable left = parse_comparable();
      CompareOp op = parse_compare_op();
      Comparable right = parse_comparable();
      auto expr = std::make_unique<Expr>();
      expr->node = Expr::Comparison{std::move(left), op, std::move(right)};
      return expr;
    }

    if (c == '$' || c == '@') {
      Query query = parse_filter_query();
      skip_ws();
      if (is_compare_op()) {
        if (!query.singular) {
          throw error("Comparison requires singular query");
        }
        CompareOp op = parse_compare_op();
        Comparable right = parse_comparable();
        Comparable left{std::move(query)};
        auto expr = std::make_unique<Expr>();
        expr->node = Expr::Comparison{std::move(left), op, std::move(right)};
        return expr;
      }
      auto expr = std::make_unique<Expr>();
      expr->node = Expr::Test{TestItem{std::move(query)}};
      return expr;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
      auto func = parse_function_expr();
      skip_ws();
      if (is_compare_op()) {
        if (func->ret != FnReturn::Value) {
          throw error("Comparison requires value-returning function");
        }
        CompareOp op = parse_compare_op();
        Comparable right = parse_comparable();
        Comparable left{std::move(func)};
        auto expr = std::make_unique<Expr>();
        expr->node = Expr::Comparison{std::move(left), op, std::move(right)};
        return expr;
      }
      if (func->ret == FnReturn::Value) {
        throw error("Value-returning function cannot be used as a test expression");
      }
      auto expr = std::make_unique<Expr>();
      expr->node = Expr::Test{TestItem{std::move(func)}};
      return expr;
    }

    throw error("Invalid expression");
  }

  bool is_compare_op() {
    skip_ws();
    return input_.substr(pos_, 2) == "==" || input_.substr(pos_, 2) == "!=" ||
           input_.substr(pos_, 2) == "<=" || input_.substr(pos_, 2) == ">=" ||
           input_.substr(pos_, 1) == "<" || input_.substr(pos_, 1) == ">";
  }

  CompareOp parse_compare_op() {
    skip_ws();
    if (consume_str("==")) {
      return CompareOp::Eq;
    }
    if (consume_str("!=")) {
      return CompareOp::Ne;
    }
    if (consume_str("<=")) {
      return CompareOp::Lte;
    }
    if (consume_str(">=")) {
      return CompareOp::Gte;
    }
    if (consume_str("<")) {
      return CompareOp::Lt;
    }
    if (consume_str(">")) {
      return CompareOp::Gt;
    }
    throw error("Expected comparison operator");
  }
};

struct ValueResult {
  bool is_nothing = false;
  const Json* ref = nullptr;
  std::optional<Json> literal;

  const Json& value() const {
    if (ref) {
      return *ref;
    }
    return *literal;
  }
};

struct EvalContext {
  const Json* root = nullptr;
  const Json* current = nullptr;
};

using NodeList = std::vector<const Json*>;

ValueResult make_literal(Json value) {
  ValueResult res;
  res.literal = std::move(value);
  return res;
}

ValueResult make_ref(const Json* node) {
  ValueResult res;
  res.ref = node;
  return res;
}

ValueResult make_nothing() {
  ValueResult res;
  res.is_nothing = true;
  return res;
}

bool compare_json_values(const Json& lhs, const Json& rhs) {
  return json_equal(lhs, rhs);
}

int64_t clamp_int64(int64_t value, int64_t min_value, int64_t max_value) {
  return std::max(min_value, std::min(value, max_value));
}

void collect_descendants(const Json* node, NodeList& out) {
  out.push_back(node);
  if (node->is_array()) {
    for (const auto& child : node->as_array()) {
      collect_descendants(child.get(), out);
    }
  } else if (node->is_object()) {
    for (const auto& [key, child] : node->as_object()) {
      (void)key;
      collect_descendants(child.get(), out);
    }
  }
}

NodeList eval_query(const Query& query, const Json* root, const Json* start);

bool eval_expr(const Expr& expr, const EvalContext& ctx);

NodeList apply_selector(const Selector& selector, const Json* node, const EvalContext& ctx) {
  NodeList out;
  if (std::holds_alternative<Selector::Name>(selector.node)) {
    if (!node->is_object()) {
      return out;
    }
    const auto& name = std::get<Selector::Name>(selector.node).value;
    const auto& obj = node->as_object();
    auto it = obj.find(name);
    if (it != obj.end()) {
      out.push_back(it->second.get());
    }
    return out;
  }
  if (std::holds_alternative<Selector::Wildcard>(selector.node)) {
    if (node->is_array()) {
      for (const auto& child : node->as_array()) {
        out.push_back(child.get());
      }
    } else if (node->is_object()) {
      for (const auto& [key, child] : node->as_object()) {
        (void)key;
        out.push_back(child.get());
      }
    }
    return out;
  }
  if (std::holds_alternative<Selector::Index>(selector.node)) {
    if (!node->is_array()) {
      return out;
    }
    const auto& arr = node->as_array();
    int64_t idx = std::get<Selector::Index>(selector.node).value;
    int64_t size = static_cast<int64_t>(arr.size());
    if (idx < 0) {
      idx = size + idx;
    }
    if (idx >= 0 && idx < size) {
      out.push_back(arr[static_cast<size_t>(idx)].get());
    }
    return out;
  }
  if (std::holds_alternative<Selector::SliceSel>(selector.node)) {
    if (!node->is_array()) {
      return out;
    }
    const auto& arr = node->as_array();
    int64_t size = static_cast<int64_t>(arr.size());
    const Slice& slice = std::get<Selector::SliceSel>(selector.node).value;
    int64_t step = slice.step.value_or(1);
    if (step == 0) {
      return out;
    }
    auto normalize = [&](int64_t idx) {
      return idx >= 0 ? idx : size + idx;
    };
    int64_t start = slice.start.has_value() ? normalize(*slice.start) : (step > 0 ? 0 : size - 1);
    int64_t end = slice.end.has_value() ? normalize(*slice.end) : (step > 0 ? size : -1);

    if (step > 0) {
      start = clamp_int64(start, 0, size);
      end = clamp_int64(end, 0, size);
      for (int64_t i = start; i < end; i += step) {
        out.push_back(arr[static_cast<size_t>(i)].get());
      }
    } else {
      start = clamp_int64(start, -1, size - 1);
      end = clamp_int64(end, -1, size - 1);
      for (int64_t i = start; i > end; i += step) {
        out.push_back(arr[static_cast<size_t>(i)].get());
      }
    }
    return out;
  }
  if (std::holds_alternative<Selector::Filter>(selector.node)) {
    const auto& filter = std::get<Selector::Filter>(selector.node);
    if (node->is_array()) {
      for (const auto& child : node->as_array()) {
        EvalContext child_ctx{ctx.root, child.get()};
        if (eval_expr(*filter.expr, child_ctx)) {
          out.push_back(child.get());
        }
      }
    } else if (node->is_object()) {
      for (const auto& [key, child] : node->as_object()) {
        (void)key;
        EvalContext child_ctx{ctx.root, child.get()};
        if (eval_expr(*filter.expr, child_ctx)) {
          out.push_back(child.get());
        }
      }
    }
    return out;
  }
  return out;
}

NodeList apply_segment(const Segment& segment, const NodeList& input, const EvalContext& ctx) {
  NodeList out;
  if (segment.descendant) {
    for (const auto* node : input) {
      NodeList descendants;
      collect_descendants(node, descendants);
      for (const auto* candidate : descendants) {
        for (const auto& selector : segment.selectors) {
          NodeList matched = apply_selector(selector, candidate, ctx);
          out.insert(out.end(), matched.begin(), matched.end());
        }
      }
    }
    return out;
  }
  for (const auto* node : input) {
    for (const auto& selector : segment.selectors) {
      NodeList matched = apply_selector(selector, node, ctx);
      out.insert(out.end(), matched.begin(), matched.end());
    }
  }
  return out;
}

NodeList eval_query(const Query& query, const Json* root, const Json* start) {
  NodeList nodes;
  nodes.push_back(start);
  EvalContext ctx{root, start};
  for (const auto& segment : query.segments) {
    nodes = apply_segment(segment, nodes, ctx);
  }
  return nodes;
}

ValueResult eval_query_value(const Query& query, const Json* root, const Json* start) {
  NodeList nodes = eval_query(query, root, start);
  if (nodes.empty()) {
    return make_nothing();
  }
  if (nodes.size() != 1) {
    throw std::runtime_error("Singular query returned multiple nodes");
  }
  return make_ref(nodes.front());
}

struct FunctionResult {
  FnReturn type;
  ValueResult value;
  bool logical = false;
};

FunctionResult eval_function(const FunctionExpr& func, const EvalContext& ctx) {
  if (func.name == "length") {
    const auto& arg = func.args[0];
    ValueResult value;
    if (std::holds_alternative<Literal>(arg)) {
      value = make_literal(std::get<Literal>(arg).value);
    } else if (std::holds_alternative<Query>(arg)) {
      value = eval_query_value(std::get<Query>(arg), ctx.root, ctx.current);
    } else if (std::holds_alternative<std::unique_ptr<FunctionExpr>>(arg)) {
      auto& fn = *std::get<std::unique_ptr<FunctionExpr>>(arg);
      FunctionResult res = eval_function(fn, ctx);
      if (res.type != FnReturn::Value) {
        throw std::runtime_error("length() expects ValueType argument");
      }
      value = res.value;
    } else {
      throw std::runtime_error("Invalid length() argument");
    }
    if (value.is_nothing) {
      return FunctionResult{FnReturn::Value, make_nothing(), false};
    }
    const Json& v = value.value();
    if (v.is_string()) {
      return FunctionResult{FnReturn::Value, make_literal(Json(static_cast<double>(v.as_string().size()))), false};
    }
    if (v.is_array()) {
      return FunctionResult{FnReturn::Value, make_literal(Json(static_cast<double>(v.as_array().size()))), false};
    }
    if (v.is_object()) {
      return FunctionResult{FnReturn::Value, make_literal(Json(static_cast<double>(v.as_object().size()))), false};
    }
    return FunctionResult{FnReturn::Value, make_nothing(), false};
  }

  if (func.name == "count") {
    const auto& arg = func.args[0];
    if (!std::holds_alternative<Query>(arg)) {
      throw std::runtime_error("count() expects NodesType argument");
    }
    NodeList nodes = eval_query(std::get<Query>(arg), ctx.root, ctx.current);
    return FunctionResult{FnReturn::Value, make_literal(Json(static_cast<double>(nodes.size()))), false};
  }

  if (func.name == "value") {
    const auto& arg = func.args[0];
    if (!std::holds_alternative<Query>(arg)) {
      throw std::runtime_error("value() expects NodesType argument");
    }
    NodeList nodes = eval_query(std::get<Query>(arg), ctx.root, ctx.current);
    if (nodes.size() != 1) {
      return FunctionResult{FnReturn::Value, make_nothing(), false};
    }
    return FunctionResult{FnReturn::Value, make_ref(nodes.front()), false};
  }

  if (func.name == "match" || func.name == "search") {
    auto eval_value_arg = [&](const std::variant<Literal, Query, std::unique_ptr<FunctionExpr>, std::unique_ptr<Expr>>& arg) {
      if (std::holds_alternative<Literal>(arg)) {
        return make_literal(std::get<Literal>(arg).value);
      }
      if (std::holds_alternative<Query>(arg)) {
        return eval_query_value(std::get<Query>(arg), ctx.root, ctx.current);
      }
      if (std::holds_alternative<std::unique_ptr<FunctionExpr>>(arg)) {
        auto& fn = *std::get<std::unique_ptr<FunctionExpr>>(arg);
        FunctionResult res = eval_function(fn, ctx);
        if (res.type != FnReturn::Value) {
          throw std::runtime_error("Function argument expected ValueType");
        }
        return res.value;
      }
      throw std::runtime_error("Invalid argument");
    };

    ValueResult arg1 = eval_value_arg(func.args[0]);
    ValueResult arg2 = eval_value_arg(func.args[1]);
    if (arg1.is_nothing || arg2.is_nothing) {
      return FunctionResult{FnReturn::Logical, make_nothing(), false};
    }
    const Json& v1 = arg1.value();
    const Json& v2 = arg2.value();
    if (!v1.is_string() || !v2.is_string()) {
      return FunctionResult{FnReturn::Logical, make_nothing(), false};
    }
    try {
      std::regex regex(v2.as_string(), std::regex::ECMAScript);
      bool matched = false;
      if (func.name == "match") {
        matched = std::regex_match(v1.as_string(), regex);
      } else {
        matched = std::regex_search(v1.as_string(), regex);
      }
      return FunctionResult{FnReturn::Logical, make_nothing(), matched};
    } catch (const std::regex_error&) {
      return FunctionResult{FnReturn::Logical, make_nothing(), false};
    }
  }

  throw std::runtime_error("Unknown function");
}

bool eval_test_item(const TestItem& item, const EvalContext& ctx) {
  if (std::holds_alternative<Query>(item.node)) {
    const Query& query = std::get<Query>(item.node);
    const Json* start = query.absolute ? ctx.root : ctx.current;
    NodeList nodes = eval_query(query, ctx.root, start);
    return !nodes.empty();
  }
  const auto& func = *std::get<std::unique_ptr<FunctionExpr>>(item.node);
  FunctionResult result = eval_function(func, ctx);
  if (result.type == FnReturn::Logical) {
    return result.logical;
  }
  throw std::runtime_error("Value-returning function cannot be used as test expression");
}

ValueResult eval_comparable(const Comparable& comp, const EvalContext& ctx) {
  if (std::holds_alternative<Literal>(comp.node)) {
    return make_literal(std::get<Literal>(comp.node).value);
  }
  if (std::holds_alternative<Query>(comp.node)) {
    const Query& query = std::get<Query>(comp.node);
    const Json* start = query.absolute ? ctx.root : ctx.current;
    return eval_query_value(query, ctx.root, start);
  }
  const auto& func = *std::get<std::unique_ptr<FunctionExpr>>(comp.node);
  FunctionResult result = eval_function(func, ctx);
  if (result.type != FnReturn::Value) {
    throw std::runtime_error("Comparable requires value-returning function");
  }
  return result.value;
}

bool compare_values(const ValueResult& lhs, const ValueResult& rhs, CompareOp op) {
  if (lhs.is_nothing || rhs.is_nothing) {
    if (op == CompareOp::Eq) {
      return lhs.is_nothing && rhs.is_nothing;
    }
    if (op == CompareOp::Ne) {
      return lhs.is_nothing != rhs.is_nothing;
    }
    return false;
  }
  const Json& left = lhs.value();
  const Json& right = rhs.value();

  if (op == CompareOp::Eq || op == CompareOp::Ne) {
    bool eq = compare_json_values(left, right);
    return op == CompareOp::Eq ? eq : !eq;
  }

  if (left.is_number() && right.is_number()) {
    if (op == CompareOp::Lt) {
      return left.as_number() < right.as_number();
    }
    if (op == CompareOp::Lte) {
      return left.as_number() <= right.as_number();
    }
    if (op == CompareOp::Gt) {
      return left.as_number() > right.as_number();
    }
    if (op == CompareOp::Gte) {
      return left.as_number() >= right.as_number();
    }
  }

  if (left.is_string() && right.is_string()) {
    if (op == CompareOp::Lt) {
      return left.as_string() < right.as_string();
    }
    if (op == CompareOp::Lte) {
      return left.as_string() <= right.as_string();
    }
    if (op == CompareOp::Gt) {
      return left.as_string() > right.as_string();
    }
    if (op == CompareOp::Gte) {
      return left.as_string() >= right.as_string();
    }
  }

  return false;
}

bool eval_expr(const Expr& expr, const EvalContext& ctx) {
  if (std::holds_alternative<Expr::Or>(expr.node)) {
    const auto& node = std::get<Expr::Or>(expr.node);
    return eval_expr(*node.left, ctx) || eval_expr(*node.right, ctx);
  }
  if (std::holds_alternative<Expr::And>(expr.node)) {
    const auto& node = std::get<Expr::And>(expr.node);
    return eval_expr(*node.left, ctx) && eval_expr(*node.right, ctx);
  }
  if (std::holds_alternative<Expr::Not>(expr.node)) {
    const auto& node = std::get<Expr::Not>(expr.node);
    return !eval_expr(*node.expr, ctx);
  }
  if (std::holds_alternative<Expr::Comparison>(expr.node)) {
    const auto& node = std::get<Expr::Comparison>(expr.node);
    ValueResult left = eval_comparable(node.left, ctx);
    ValueResult right = eval_comparable(node.right, ctx);
    return compare_values(left, right, node.op);
  }
  const auto& node = std::get<Expr::Test>(expr.node);
  return eval_test_item(node.item, ctx);
}

}  // namespace

struct JsonPath::Impl {
  Query query;
};

JsonPath::JsonPath(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

JsonPath JsonPath::compile(std::string_view path) {
  JsonPathParser parser(path);
  Query query = parser.parse_query(true);
  parser.ensure_end();
  auto impl = std::make_shared<Impl>();
  impl->query = std::move(query);
  return JsonPath(std::move(impl));
}

std::vector<const Json*> JsonPath::select(const Json& root) const {
  if (!impl_) {
    throw std::runtime_error("JsonPath is not compiled");
  }
  const Json* start = &root;
  NodeList nodes = eval_query(impl_->query, &root, start);
  return nodes;
}

std::vector<const Json*> select(const Json& root, std::string_view path) {
  JsonPath compiled = JsonPath::compile(path);
  return compiled.select(root);
}

}  // namespace jsonpath
