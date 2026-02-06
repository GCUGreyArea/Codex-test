#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "jsonpath/json.hpp"

namespace jsonpath {

class JsonPath {
 public:
  static JsonPath compile(std::string_view path);

  std::vector<const Json*> select(const Json& root) const;

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;

  explicit JsonPath(std::shared_ptr<const Impl> impl);
};

std::vector<const Json*> select(const Json& root, std::string_view path);

}  // namespace jsonpath
