#pragma once
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

namespace nexus {

// Strict numeric command-line values: the whole argument must be a number in range for T,
// otherwise the program exits with a message instead of silently using 0, a prefix such as
// "10" from "10junk", or a wrapped negative.
template <class T>
T parse_flag(const std::string& flag, const std::string& value) {
  T out{};
  bool ok = !value.empty();
  if constexpr (std::is_floating_point_v<T>) {
    char* end = nullptr;
    errno = 0;
    out = static_cast<T>(std::strtod(value.c_str(), &end));
    ok = ok && *end == '\0' && errno != ERANGE && std::isfinite(out);
  } else {
    const char* last = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), last, out);
    ok = ok && ec == std::errc{} && ptr == last;
  }
  if (!ok) {
    std::fprintf(stderr, "invalid value for %s: '%s'\n", flag.c_str(), value.c_str());
    std::exit(2);
  }
  return out;
}

}  // namespace nexus
