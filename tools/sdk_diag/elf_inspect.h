// elf_inspect — minimal ELF64 dynamic-section reader built on <elf.h>.
// No libelf / no external linkage; parses section headers directly.
#ifndef SDK_DIAG_ELF_INSPECT_H
#define SDK_DIAG_ELF_INSPECT_H

#include <cstdint>
#include <string>
#include <vector>

namespace sdk_diag {

struct ElfInfo {
  bool ok = false;        // false on parse error (see error)
  std::string error;

  uint16_t machine = 0;   // e_machine (EM_X86_64 = 62, EM_AARCH64 = 183)
  std::string arch_name;  // "x86_64" / "aarch64" / "" if other

  bool has_rpath = false;
  bool has_runpath = false;
  std::string runpath;    // DT_RUNPATH value (preferred); falls back to DT_RPATH

  std::vector<std::string> needed;      // DT_NEEDED entries
  std::vector<std::string> exported;    // defined, non-local dynamic symbols
  std::vector<std::string> undefined;   // undefined dynamic symbols (imports)

  // Highest required versioned-symbol tags, e.g. "2.35" and "3.4.30".
  std::string max_glibc;
  std::string max_glibcxx;
};

// Parses the ELF shared object / executable at `path`.
ElfInfo InspectElf(const std::string& path);

// Compares two dotted version strings ("2.35", "3.4.30"). Returns
// -1/0/1 like strcmp on numeric components.
int CompareVersion(const std::string& a, const std::string& b);

}  // namespace sdk_diag

#endif  // SDK_DIAG_ELF_INSPECT_H
