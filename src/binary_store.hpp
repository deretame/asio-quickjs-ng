#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct Host;

// Global process-wide binary blob store shared by all Host instances.
// put(bytes) -> id; take(id) -> bytes (consumes) or null if missing/expired.
// Entries unused for the configured TTL (default 15 minutes) are dropped by
// lazy GC on put/take.
namespace binary_store {

std::string put(std::span<const uint8_t> data);
std::optional<std::vector<uint8_t>> take(std::string_view id);

// Drop expired entries. Called automatically from put/take.
void gc();

// Test helpers.
void clear();
void set_ttl(std::chrono::milliseconds ttl);
std::chrono::milliseconds ttl();
std::size_t size();

bool install(Host& host);

}  // namespace binary_store
