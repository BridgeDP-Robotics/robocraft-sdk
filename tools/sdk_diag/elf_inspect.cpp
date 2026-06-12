#include "elf_inspect.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <sstream>

namespace sdk_diag {
namespace {

// Bounds-checked view over the mmapped file.
struct Image {
  const uint8_t* base = nullptr;
  std::size_t size = 0;

  bool InRange(std::size_t off, std::size_t len) const {
    return off <= size && len <= size - off;
  }
  template <typename T>
  const T* At(std::size_t off) const {
    if (!InRange(off, sizeof(T))) return nullptr;
    return reinterpret_cast<const T*>(base + off);
  }
  // Reads a NUL-terminated string at strtab section [str_off, str_off+str_sz)
  // with index `idx`. Returns empty on out-of-range.
  std::string Str(std::size_t str_off, std::size_t str_sz, uint64_t idx) const {
    if (idx >= str_sz) return {};
    std::size_t start = str_off + idx;
    std::string out;
    for (std::size_t i = start; i < str_off + str_sz && i < size; ++i) {
      char c = static_cast<char>(base[i]);
      if (c == '\0') break;
      out += c;
    }
    return out;
  }
};

std::string ExtractVersionTag(const std::string& sym, const char* prefix) {
  std::size_t plen = std::strlen(prefix);
  if (sym.compare(0, plen, prefix) == 0) return sym.substr(plen);
  return {};
}

}  // namespace

int CompareVersion(const std::string& a, const std::string& b) {
  std::size_t ia = 0, ib = 0;
  while (ia < a.size() || ib < b.size()) {
    long va = 0, vb = 0;
    while (ia < a.size() && a[ia] != '.') {
      if (a[ia] >= '0' && a[ia] <= '9') va = va * 10 + (a[ia] - '0');
      ++ia;
    }
    while (ib < b.size() && b[ib] != '.') {
      if (b[ib] >= '0' && b[ib] <= '9') vb = vb * 10 + (b[ib] - '0');
      ++ib;
    }
    if (va != vb) return va < vb ? -1 : 1;
    if (ia < a.size()) ++ia;
    if (ib < b.size()) ++ib;
  }
  return 0;
}

ElfInfo InspectElf(const std::string& path) {
  ElfInfo info;

  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    info.error = "cannot open: " + path;
    return info;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
    info.error = "cannot stat: " + path;
    ::close(fd);
    return info;
  }
  std::size_t fsize = static_cast<std::size_t>(st.st_size);
  void* map = ::mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED) {
    info.error = "mmap failed: " + path;
    return info;
  }

  Image img;
  img.base = static_cast<const uint8_t*>(map);
  img.size = fsize;

  auto fail = [&](const std::string& msg) -> ElfInfo& {
    info.ok = false;
    info.error = msg;
    return info;
  };

  const Elf64_Ehdr* eh = img.At<Elf64_Ehdr>(0);
  if (!eh || std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
    ::munmap(map, fsize);
    return fail("not an ELF file");
  }
  if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
    ::munmap(map, fsize);
    return fail("only 64-bit ELF is supported");
  }

  info.machine = eh->e_machine;
  if (eh->e_machine == EM_X86_64)
    info.arch_name = "x86_64";
  else if (eh->e_machine == EM_AARCH64)
    info.arch_name = "aarch64";

  if (eh->e_shoff == 0 || eh->e_shnum == 0) {
    ::munmap(map, fsize);
    return fail("no section headers (stripped?)");
  }

  std::size_t shoff = eh->e_shoff;
  std::size_t shentsize = eh->e_shentsize ? eh->e_shentsize : sizeof(Elf64_Shdr);
  std::size_t shnum = eh->e_shnum;
  if (!img.InRange(shoff, shentsize * shnum)) {
    ::munmap(map, fsize);
    return fail("section header table out of range");
  }

  auto shdr = [&](std::size_t i) -> const Elf64_Shdr* {
    return img.At<Elf64_Shdr>(shoff + i * shentsize);
  };

  // Locate .dynamic, .dynsym, and .gnu.version_r.
  const Elf64_Shdr* dyn_sh = nullptr;
  const Elf64_Shdr* dynsym_sh = nullptr;
  const Elf64_Shdr* verneed_sh = nullptr;
  for (std::size_t i = 0; i < shnum; ++i) {
    const Elf64_Shdr* s = shdr(i);
    if (!s) continue;
    if (s->sh_type == SHT_DYNAMIC)
      dyn_sh = s;
    else if (s->sh_type == SHT_DYNSYM)
      dynsym_sh = s;
    else if (s->sh_type == SHT_GNU_verneed)
      verneed_sh = s;
  }

  // .dynamic — DT_NEEDED / DT_RPATH / DT_RUNPATH (strings live in sh_link strtab).
  if (dyn_sh && dyn_sh->sh_link < shnum) {
    const Elf64_Shdr* strs = shdr(dyn_sh->sh_link);
    if (strs && img.InRange(dyn_sh->sh_offset, dyn_sh->sh_size) &&
        img.InRange(strs->sh_offset, strs->sh_size)) {
      std::size_t n = dyn_sh->sh_entsize ? dyn_sh->sh_size / dyn_sh->sh_entsize : 0;
      for (std::size_t i = 0; i < n; ++i) {
        const Elf64_Dyn* d = img.At<Elf64_Dyn>(dyn_sh->sh_offset + i * sizeof(Elf64_Dyn));
        if (!d) break;
        if (d->d_tag == DT_NULL) break;
        if (d->d_tag == DT_NEEDED) {
          info.needed.push_back(img.Str(strs->sh_offset, strs->sh_size, d->d_un.d_val));
        } else if (d->d_tag == DT_RPATH) {
          info.has_rpath = true;
          if (info.runpath.empty())
            info.runpath = img.Str(strs->sh_offset, strs->sh_size, d->d_un.d_val);
        } else if (d->d_tag == DT_RUNPATH) {
          info.has_runpath = true;
          info.runpath = img.Str(strs->sh_offset, strs->sh_size, d->d_un.d_val);
        }
      }
    }
  }

  // .dynsym — exported (defined global/weak) and undefined (imported) symbols.
  if (dynsym_sh && dynsym_sh->sh_link < shnum) {
    const Elf64_Shdr* strs = shdr(dynsym_sh->sh_link);
    if (strs && img.InRange(dynsym_sh->sh_offset, dynsym_sh->sh_size) &&
        img.InRange(strs->sh_offset, strs->sh_size) && dynsym_sh->sh_entsize) {
      std::size_t n = dynsym_sh->sh_size / dynsym_sh->sh_entsize;
      for (std::size_t i = 1; i < n; ++i) {  // skip null symbol 0
        const Elf64_Sym* sym = img.At<Elf64_Sym>(dynsym_sh->sh_offset + i * sizeof(Elf64_Sym));
        if (!sym) break;
        std::string name = img.Str(strs->sh_offset, strs->sh_size, sym->st_name);
        if (name.empty()) continue;
        unsigned bind = ELF64_ST_BIND(sym->st_info);
        if (sym->st_shndx == SHN_UNDEF) {
          info.undefined.push_back(name);
        } else if (bind == STB_GLOBAL || bind == STB_WEAK) {
          info.exported.push_back(name);
        }
      }
    }
  }

  // .gnu.version_r — required versioned-symbol tags (GLIBC_x.y / GLIBCXX_x.y).
  if (verneed_sh && verneed_sh->sh_link < shnum) {
    const Elf64_Shdr* strs = shdr(verneed_sh->sh_link);
    if (strs && img.InRange(verneed_sh->sh_offset, verneed_sh->sh_size) &&
        img.InRange(strs->sh_offset, strs->sh_size)) {
      std::size_t vn_off = verneed_sh->sh_offset;
      const std::size_t vn_end = verneed_sh->sh_offset + verneed_sh->sh_size;
      while (vn_off + sizeof(Elf64_Verneed) <= vn_end) {
        const Elf64_Verneed* vn = img.At<Elf64_Verneed>(vn_off);
        if (!vn) break;
        std::size_t aux_off = vn_off + vn->vn_aux;
        for (unsigned a = 0; a < vn->vn_cnt; ++a) {
          const Elf64_Vernaux* aux = img.At<Elf64_Vernaux>(aux_off);
          if (!aux) break;
          std::string tag = img.Str(strs->sh_offset, strs->sh_size, aux->vna_name);
          std::string g = ExtractVersionTag(tag, "GLIBC_");
          if (!g.empty() && (info.max_glibc.empty() || CompareVersion(g, info.max_glibc) > 0))
            info.max_glibc = g;
          std::string gx = ExtractVersionTag(tag, "GLIBCXX_");
          if (!gx.empty() && (info.max_glibcxx.empty() || CompareVersion(gx, info.max_glibcxx) > 0))
            info.max_glibcxx = gx;
          if (aux->vna_next == 0) break;
          aux_off += aux->vna_next;
        }
        if (vn->vn_next == 0) break;
        vn_off += vn->vn_next;
      }
    }
  }

  ::munmap(map, fsize);
  info.ok = true;
  return info;
}

}  // namespace sdk_diag
