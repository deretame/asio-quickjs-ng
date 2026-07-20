// Host ID tests: each Host instance gets a unique ID (UUID v4 by default).
#include <gtest/gtest.h>

#include <set>
#include <string>

#include "host.hpp"
#include "qjs.hpp"

namespace {

bool setup_host(Host &host)
{
  if (!host) {
    return false;
  }
  return host.install_runtime();
}

std::string js_str(qjs::Value v)
{
  auto s = v.to_std_string();
  return s.value_or("");
}

}  // namespace

TEST(HostId, PerInstanceIdIsUnique) {
  Host h1;
  Host h2;
  Host h3;
  ASSERT_TRUE(h1);
  ASSERT_TRUE(h2);
  ASSERT_TRUE(h3);

  EXPECT_FALSE(h1.id().empty());
  EXPECT_FALSE(h2.id().empty());
  EXPECT_FALSE(h3.id().empty());

  std::set<std::string> ids;
  ids.insert(h1.id());
  ids.insert(h2.id());
  ids.insert(h3.id());
  EXPECT_EQ(ids.size(), 3u);
}

TEST(HostId, DefaultIdIsUuidV4Shape) {
  Host host;
  ASSERT_TRUE(host);

  const std::string &id = host.id();
  ASSERT_EQ(id.length(), 36u);
  EXPECT_EQ(id[8], '-');
  EXPECT_EQ(id[13], '-');
  EXPECT_EQ(id[14], '4');  // version nibble
  EXPECT_EQ(id[18], '-');
  // Variant RFC 4122: byte 8's top two bits are 10 -> high nibble is 8/9/a/b.
  char variant_nibble = id[19];
  EXPECT_TRUE(
    variant_nibble == '8' || variant_nibble == '9'
    || variant_nibble == 'a' || variant_nibble == 'b')
    << "variant nibble was " << variant_nibble;
  EXPECT_EQ(id[23], '-');
}

TEST(HostId, CustomIdCanBeProvided) {
  Host host("my-custom-instance-id");
  ASSERT_TRUE(host);

  EXPECT_EQ(host.id(), "my-custom-instance-id");

  ASSERT_TRUE(setup_host(host));
  ASSERT_TRUE(
    host.eval_source(
    R"JS(
        globalThis.__customId = globalThis.__hostID;
      )JS",
    "custom_id.js"));
  EXPECT_EQ(js_str(host.global().get("__customId")), "my-custom-instance-id");
}

TEST(HostId, IdIsExposedToJs) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
        globalThis.__jsId = globalThis.__hostID;
      )JS",
    "id_exposed.js"));

  std::string cpp_id = host.id();
  std::string js_id = js_str(host.global().get("__jsId"));
  EXPECT_EQ(js_id, cpp_id);
}

TEST(HostId, MultipleInstancesExposeDifferentIdsToJs) {
  Host a;
  Host b;
  ASSERT_TRUE(setup_host(a));
  ASSERT_TRUE(setup_host(b));

  ASSERT_TRUE(a.eval_source("globalThis.__id = globalThis.__hostID;", "a.js"));
  ASSERT_TRUE(b.eval_source("globalThis.__id = globalThis.__hostID;", "b.js"));

  std::string id_a = js_str(a.global().get("__id"));
  std::string id_b = js_str(b.global().get("__id"));

  EXPECT_FALSE(id_a.empty());
  EXPECT_FALSE(id_b.empty());
  EXPECT_NE(id_a, id_b);
}
