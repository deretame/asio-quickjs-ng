#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <rfl.hpp>
#include <rfl/json.hpp>

#include <string>

namespace {

struct Person {
  std::string name;
  int age = 0;
};

} // namespace

TEST(NlohmannJson, ParseDynamic) {
  using json = nlohmann::json;
  auto j = json::parse(R"({"name":"Ada","tags":["a","b"],"n":42})");
  EXPECT_EQ(j.at("name").get<std::string>(), "Ada");
  EXPECT_EQ(j.at("n").get<int>(), 42);
  EXPECT_EQ(j.at("tags").size(), 2u);
  EXPECT_EQ(j.value("missing", 7), 7);
}

TEST(ReflectCpp, StructRoundTrip) {
  Person p{.name = "Grace", .age = 36};
  const std::string s = rfl::json::write(p);
  auto back = rfl::json::read<Person>(s);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->name, "Grace");
  EXPECT_EQ(back->age, 36);
}
