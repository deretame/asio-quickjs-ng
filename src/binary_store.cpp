#include "binary_store.hpp"

#include "host.hpp"
#include "qjs.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <utility>

namespace binary_store {

namespace {

struct Entry {
  std::vector<uint8_t> data;
  std::chrono::steady_clock::time_point last_used;
};

struct Store {
  std::mutex mutex;
  std::unordered_map<std::string, Entry> map;
  std::chrono::milliseconds ttl{std::chrono::minutes(15)};
};

Store& store()
{
  static Store s;
  return s;
}

void gc_locked(Store& s, std::chrono::steady_clock::time_point now)
{
  for (auto it = s.map.begin(); it != s.map.end();) {
    if (now - it->second.last_used >= s.ttl) {
      it = s.map.erase(it);
    } else {
      ++it;
    }
  }
}

std::string make_id()
{
  boost::uuids::random_generator gen;
  return boost::uuids::to_string(gen());
}

qjs::Value put_fn(qjs::Ctx ctx, std::span<const uint8_t> data)
{
  return ctx.new_string(put(data));
}

qjs::Value take_fn(qjs::Ctx ctx, std::string id)
{
  auto data = take(id);
  if (!data) {
    return qjs::Value::take(ctx.get(), JS_UNDEFINED);
  }
  return qjs::new_uint8_array(ctx.get(), std::move(*data));
}

}  // namespace

std::string put(std::span<const uint8_t> data)
{
  auto& s = store();
  auto now = std::chrono::steady_clock::now();
  std::string id = make_id();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    gc_locked(s, now);
    Entry entry;
    entry.data.assign(data.begin(), data.end());
    entry.last_used = now;
    s.map.emplace(id, std::move(entry));
  }
  return id;
}

std::optional<std::vector<uint8_t>> take(std::string_view id)
{
  auto& s = store();
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(s.mutex);
  gc_locked(s, now);
  auto it = s.map.find(std::string(id));
  if (it == s.map.end()) {
    return std::nullopt;
  }
  std::vector<uint8_t> out = std::move(it->second.data);
  s.map.erase(it);
  return out;
}

void gc()
{
  auto& s = store();
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(s.mutex);
  gc_locked(s, now);
}

void clear()
{
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.map.clear();
}

void set_ttl(std::chrono::milliseconds ttl)
{
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.ttl = ttl;
}

std::chrono::milliseconds ttl()
{
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.ttl;
}

std::size_t size()
{
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.map.size();
}

bool install(Host& host)
{
  auto g = host.global();
  g.obj(
    "binaryStore",
    [](qjs::Value& o) {
      o.fn<&put_fn>("put");
      o.fn<&take_fn>("take");
    });
  return true;
}

}  // namespace binary_store
