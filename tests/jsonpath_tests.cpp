#include "jsonpath/jsonpath.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

jsonpath::Json parse_doc() {
  const char* doc = R"JSON(
  {
    "name": "Barry",
    "tags": ["a", "b", "c"],
    "numbers": [1, 2, 3, 4, 5, 6],
    "items": [
      {"id": 1, "b": "j", "colors": ["red", "green"], "author": "Bob", "date": "1974-05-10"},
      {"id": 2, "b": "k", "colors": ["blue"], "author": "Rob", "date": "1976-05-10"},
      {"id": 3, "b": {}, "colors": [], "author": "Alice", "date": "1974-07-11"},
      {"id": 4, "b": "kilo", "colors": ["orange", "red"], "author": "Bob", "date": "1975-02-01"}
    ],
    "nested": {"obj": {"b": "deep"}},
    "array": [{"a": 1}, {"a": 2}, {"a": 3}],
    "empty": [],
    "misc": {"a": null, "b": true, "c": false}
  }
  )JSON";
  return jsonpath::parse_json(doc);
}

bool contains_value(const std::vector<const jsonpath::Json*>& nodes, const jsonpath::Json& expected) {
  return std::any_of(nodes.begin(), nodes.end(), [&](const jsonpath::Json* node) {
    return jsonpath::json_equal(*node, expected);
  });
}

const jsonpath::Json* find_item_by_id(const std::vector<const jsonpath::Json*>& nodes, int id) {
  for (const auto* node : nodes) {
    if (!node->is_object()) {
      continue;
    }
    const auto& obj = node->as_object();
    auto it = obj.find("id");
    if (it != obj.end() && it->second->is_number() && it->second->as_number() == id) {
      return node;
    }
  }
  return nullptr;
}

}  // namespace

TEST(JsonParser, ParsesStringsAndNumbers) {
  auto doc = jsonpath::parse_json(R"JSON({"a": "b\\n", "n": 3.5, "t": true, "f": false, "z": null})JSON");
  ASSERT_TRUE(doc.is_object());
  const auto& obj = doc.as_object();
  EXPECT_EQ(obj.at("a")->as_string(), "b\\n");
  EXPECT_EQ(obj.at("n")->as_number(), 3.5);
  EXPECT_TRUE(obj.at("t")->as_bool());
  EXPECT_FALSE(obj.at("f")->as_bool());
  EXPECT_TRUE(obj.at("z")->is_null());
}

TEST(JsonPath, BasicSelectors) {
  auto doc = parse_doc();

  auto name = jsonpath::select(doc, "$.name");
  ASSERT_EQ(name.size(), 1u);
  EXPECT_EQ(name[0]->as_string(), "Barry");

  auto tags = jsonpath::select(doc, "$.tags[*]");
  ASSERT_EQ(tags.size(), 3u);
  EXPECT_EQ(tags[0]->as_string(), "a");
  EXPECT_EQ(tags[1]->as_string(), "b");
  EXPECT_EQ(tags[2]->as_string(), "c");

  auto last = jsonpath::select(doc, "$.tags[-1]");
  ASSERT_EQ(last.size(), 1u);
  EXPECT_EQ(last[0]->as_string(), "c");

  auto slice = jsonpath::select(doc, "$.tags[0:2]");
  ASSERT_EQ(slice.size(), 2u);
  EXPECT_EQ(slice[0]->as_string(), "a");
  EXPECT_EQ(slice[1]->as_string(), "b");

  auto duplicate = jsonpath::select(doc, "$.tags[0,0]");
  ASSERT_EQ(duplicate.size(), 2u);
  EXPECT_EQ(duplicate[0]->as_string(), "a");
  EXPECT_EQ(duplicate[1]->as_string(), "a");
}

TEST(JsonPath, DescendantAndWildcard) {
  auto doc = parse_doc();
  auto values = jsonpath::select(doc, "$..b");
  EXPECT_EQ(values.size(), 6u);
  EXPECT_TRUE(contains_value(values, jsonpath::Json("j")));
  EXPECT_TRUE(contains_value(values, jsonpath::Json("k")));
  EXPECT_TRUE(contains_value(values, jsonpath::Json("kilo")));
  EXPECT_TRUE(contains_value(values, jsonpath::Json("deep")));
  EXPECT_TRUE(contains_value(values, jsonpath::Json(true)));
  EXPECT_TRUE(contains_value(values, jsonpath::Json(jsonpath::Json::Object{})));

  auto misc = jsonpath::select(doc, "$.misc.*");
  EXPECT_EQ(misc.size(), 3u);
  EXPECT_TRUE(contains_value(misc, jsonpath::Json(nullptr)));
  EXPECT_TRUE(contains_value(misc, jsonpath::Json(true)));
  EXPECT_TRUE(contains_value(misc, jsonpath::Json(false)));
}

TEST(JsonPath, FilterSelectors) {
  auto doc = parse_doc();

  auto b_kilo = jsonpath::select(doc, "$.items[?@.b == 'kilo']");
  ASSERT_EQ(b_kilo.size(), 1u);
  EXPECT_NE(find_item_by_id(b_kilo, 4), nullptr);

  auto b_exists = jsonpath::select(doc, "$.items[?@.b]");
  EXPECT_EQ(b_exists.size(), 4u);

  auto id_ge_2 = jsonpath::select(doc, "$.items[?@.id >= 2]");
  EXPECT_EQ(id_ge_2.size(), 3u);
  EXPECT_NE(find_item_by_id(id_ge_2, 2), nullptr);
  EXPECT_NE(find_item_by_id(id_ge_2, 3), nullptr);
  EXPECT_NE(find_item_by_id(id_ge_2, 4), nullptr);
}

TEST(JsonPath, SliceWithNegativeStep) {
  auto doc = parse_doc();
  auto slice = jsonpath::select(doc, "$.numbers[4:1:-2]");
  ASSERT_EQ(slice.size(), 2u);
  EXPECT_EQ(slice[0]->as_number(), 5);
  EXPECT_EQ(slice[1]->as_number(), 3);
}

TEST(JsonPath, Functions) {
  auto doc = parse_doc();

  auto length_sel = jsonpath::select(doc, "$.items[?length(@.colors) >= 2]");
  EXPECT_EQ(length_sel.size(), 2u);
  EXPECT_NE(find_item_by_id(length_sel, 1), nullptr);
  EXPECT_NE(find_item_by_id(length_sel, 4), nullptr);

  auto count_sel = jsonpath::select(doc, "$.items[?count(@.colors[*]) == 1]");
  ASSERT_EQ(count_sel.size(), 1u);
  EXPECT_NE(find_item_by_id(count_sel, 2), nullptr);

  auto match_sel = jsonpath::select(doc, "$.items[?match(@.date, \"1974-05-..\")]");
  ASSERT_EQ(match_sel.size(), 1u);
  EXPECT_NE(find_item_by_id(match_sel, 1), nullptr);

  auto search_sel = jsonpath::select(doc, "$.items[?search(@.author, \"[BR]ob\")]");
  EXPECT_EQ(search_sel.size(), 3u);

  auto value_sel = jsonpath::select(doc, "$.items[?value(@..colors[0]) == \"red\"]");
  ASSERT_EQ(value_sel.size(), 1u);
  EXPECT_NE(find_item_by_id(value_sel, 1), nullptr);
}

TEST(JsonPath, ComparisonWithMissingNodes) {
  auto doc = jsonpath::parse_json(R"JSON([{"x": 1}])JSON");

  auto missing_eq = jsonpath::select(doc, "$[?$.missing == $.also_missing]");
  EXPECT_EQ(missing_eq.size(), 1u);

  auto missing_ne = jsonpath::select(doc, "$[?$.missing != \"x\"]");
  EXPECT_EQ(missing_ne.size(), 1u);

  auto missing_false = jsonpath::select(doc, "$[?$.missing == \"x\"]");
  EXPECT_EQ(missing_false.size(), 0u);
}

TEST(JsonPath, InvalidQueriesThrow) {
  auto doc = parse_doc();

  EXPECT_THROW(jsonpath::select(doc, "$.items[?foo(@) == 1]"), std::runtime_error);
  EXPECT_THROW(jsonpath::select(doc, "$.items[?value(@.id)]"), std::runtime_error);
  EXPECT_THROW(jsonpath::select(doc, "$.items[?length(@.*) > 1]"), std::runtime_error);
  EXPECT_THROW(jsonpath::select(doc, "$.items[?match(@.author, \"Bob\") == true]"), std::runtime_error);
}
