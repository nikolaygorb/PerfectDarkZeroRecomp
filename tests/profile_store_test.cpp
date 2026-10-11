#include "../src/profile_store.h"
#include <cassert>
#include <sstream>
#include <iostream>
using namespace pdz_profiles;
int main() {
  Store s; s.profiles[0].name = "Moka";
  auto second = AddProfile(s, "Second Agent");
  assert(second != 0 && s.profiles[0].id == 0 && s.selected == second);
  std::stringstream out; WriteStore(out, s); const auto restored = ReadStore(out);
  assert(restored.selected == second && restored.profiles[0].name == "Moka" && restored.profiles[1].name == "Second Agent");
  auto rejects = [](auto action) { try { action(); return false; } catch (const std::exception&) { return true; } };
  assert(rejects([&] { AddProfile(s, "moka"); }));
  assert(s.profiles.size() == 2 && s.selected == second);
  for (const std::string name : {"", " ", " Moka", "Moka ", "../Moka", "C:\\Moka", "Moka\nAgent", "1234567890123456"})
    assert(!ValidName(name));
  assert(ValidName("Agent_007-Ready"));
  for (const std::string data : {"PDZ_LOCAL_PROFILES 2\n0\n0 \"Moka\"", "PDZ_LOCAL_PROFILES 1\n5\n0 \"Moka\"", "PDZ_LOCAL_PROFILES 1\n0\n0 \"Moka\"\n0 \"Other\"", "PDZ_LOCAL_PROFILES 1\n0\n0 \"unfinished"})
    assert(rejects([&] { std::istringstream in(data); ReadStore(in); }));
  std::cout << "Profile persistence, isolation IDs and validation passed.\n";
}
