#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace jsonpath {

struct Json {
  using Object = std::unordered_map<std::string, std::shared_ptr<Json>>;
  using Array = std::vector<std::shared_ptr<Json>>;
  using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  Value value;

  Json() : value(nullptr) {}
  explicit Json(std::nullptr_t) : value(nullptr) {}
  explicit Json(bool b) : value(b) {}
  explicit Json(double n) : value(n) {}
  explicit Json(std::string s) : value(std::move(s)) {}
  explicit Json(const char* s) : value(std::string(s)) {}
  explicit Json(Array a) : value(std::move(a)) {}
  explicit Json(Object o) : value(std::move(o)) {}

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(value); }
  bool is_bool() const { return std::holds_alternative<bool>(value); }
  bool is_number() const { return std::holds_alternative<double>(value); }
  bool is_string() const { return std::holds_alternative<std::string>(value); }
  bool is_array() const { return std::holds_alternative<Array>(value); }
  bool is_object() const { return std::holds_alternative<Object>(value); }

  bool as_bool() const { return std::get<bool>(value); }
  double as_number() const { return std::get<double>(value); }
  const std::string& as_string() const { return std::get<std::string>(value); }
  const Array& as_array() const { return std::get<Array>(value); }
  const Object& as_object() const { return std::get<Object>(value); }
  Array& as_array() { return std::get<Array>(value); }
  Object& as_object() { return std::get<Object>(value); }
};

Json parse_json(std::string_view input);

bool json_equal(const Json& lhs, const Json& rhs);

}  // namespace jsonpath
