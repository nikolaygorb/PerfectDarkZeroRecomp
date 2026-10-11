#pragma once
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdz_profiles {
struct Profile { uint32_t id; std::string name; };
struct Store { uint32_t selected = 0; std::vector<Profile> profiles{{0, "User"}}; };
inline bool ValidName(const std::string& name) {
  if (name.empty() || name.size() > 15 || name.front() == ' ' || name.back() == ' ') return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
  });
}
inline std::string Fold(std::string value) {
  for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
inline void Validate(const Store& store) {
  if (store.profiles.empty() || store.profiles.size() > 64) throw std::runtime_error("Invalid profile count.");
  bool selected = false, legacy = false;
  for (size_t i = 0; i < store.profiles.size(); ++i) {
    const auto& p = store.profiles[i];
    if (!ValidName(p.name)) throw std::runtime_error("Use 1-15 letters, numbers, spaces, - or _.");
    selected |= p.id == store.selected;
    legacy |= p.id == 0;
    for (size_t j = 0; j < i; ++j)
      if (p.id == store.profiles[j].id || Fold(p.name) == Fold(store.profiles[j].name))
        throw std::runtime_error("A profile with that name already exists.");
  }
  if (!selected || !legacy) throw std::runtime_error("Invalid selected or original profile.");
}
inline Store ReadStore(std::istream& in) {
  Store result; result.profiles.clear();
  std::string magic; unsigned version = 0;
  if (!(in >> magic >> version >> result.selected) || magic != "PDZ_LOCAL_PROFILES" || version != 1)
    throw std::runtime_error("Unrecognized profile file. Existing saves have not been changed.");
  while (in >> std::ws && in.peek() != std::char_traits<char>::eof()) {
    Profile p{};
    if (!(in >> p.id >> std::quoted(p.name))) throw std::runtime_error("Incomplete profile file.");
    result.profiles.push_back(std::move(p));
    if (result.profiles.size() > 64) throw std::runtime_error("Too many profiles.");
  }
  Validate(result); return result;
}
inline void WriteStore(std::ostream& out, const Store& store) {
  Validate(store);
  out << "PDZ_LOCAL_PROFILES 1\n" << store.selected << '\n';
  for (const auto& p : store.profiles) out << p.id << ' ' << std::quoted(p.name) << '\n';
}
inline uint32_t AddProfile(Store& store, const std::string& name) {
  uint32_t id = 1;
  while (std::any_of(store.profiles.begin(), store.profiles.end(), [id](const Profile& p) { return p.id == id; })) ++id;
  Store updated = store;
  updated.profiles.push_back({id, name}); updated.selected = id;
  Validate(updated); store = std::move(updated); return id;
}
} // namespace pdz_profiles
