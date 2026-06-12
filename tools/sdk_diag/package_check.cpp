#include "package_check.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "elf_inspect.h"
#include "manifest.h"
#include "sha256.h"
#include "yaml_lite.h"

namespace sdk_diag {
namespace {

// ---- system-library whitelist (manual §3.3) ----
const std::array<const char*, 11> kSysLibs = {
    "libc.so.6",          "libm.so.6",          "libdl.so.2",
    "librt.so.1",         "libpthread.so.0",    "libstdc++.so.6",
    "libgcc_s.so.1",      "linux-vdso.so.1",    "ld-linux-x86-64.so.2",
    "ld-linux-aarch64.so.1", "libresolv.so.2"};

bool IsSysLib(const std::string& name) {
  for (const char* s : kSysLibs) {
    if (name == s) return true;
  }
  return false;
}

// ---- forbidden imported symbols (manual §7) ----
const std::array<const char*, 28> kForbidden = {
    "signal",   "sigaction", "fork",        "vfork",   "clone",      "execve",
    "execv",    "execvp",    "execvpe",     "execl",   "execlp",     "execle",
    "system",   "popen",     "setsid",      "daemon",  "posix_spawn", "posix_spawnp",
    "ptrace",   "chroot",    "setns",       "unshare", "reboot",     "mount",
    "umount",   "umount2",   "iopl",        "ioperm"};

bool IsForbidden(const std::string& sym) {
  for (const char* f : kForbidden) {
    if (sym == f) return true;
  }
  return false;
}

// Linker-internal defined symbols that may legitimately appear and are not a
// symbol-leak concern.
const std::array<const char*, 6> kIgnoredExports = {
    "_init", "_fini", "__bss_start", "_edata", "_end", "__gmon_start__"};

bool IsIgnoredExport(const std::string& s) {
  for (const char* k : kIgnoredExports) {
    if (s == k) return true;
  }
  return false;
}

std::string Hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%04x", v);
  return buf;
}

std::string BaseName(const std::string& p) {
  std::size_t slash = p.find_last_of('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Matches "*.so" and versioned "*.so.<n>[...]" names.
bool HasSoSuffix(const std::string& name) {
  std::size_t p = name.find(".so");
  if (p == std::string::npos) return false;
  std::size_t after = p + 3;
  return after == name.size() || name[after] == '.';
}

std::string JoinPath(const std::string& dir, const std::string& rel) {
  if (dir.empty()) return rel;
  if (dir.back() == '/') return dir + rel;
  return dir + "/" + rel;
}

bool FileExists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDir(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool EndsWith(const std::string& s, const char* suffix) {
  std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool FileSize(const std::string& path, uint64_t& out) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  out = static_cast<uint64_t>(st.st_size);
  return true;
}

// ---- i18n ----

// Picks the Chinese or English variant of a literal. Source is UTF-8; both
// arguments must be valid UTF-8 so output stays UTF-8 regardless of language.
inline const char* T(Lang lang, const char* zh, const char* en) {
  return lang == Lang::En ? en : zh;
}

// Readable name for each check id, in both languages. The English id is always
// kept in the output (right after the [PASS]/[WARN]/[FAIL] tag) so CI greps and
// tests stay stable, while the readable name makes the report easy to scan.
const char* CheckName(const std::string& id, Lang lang) {
  if (id == "structure") return T(lang, "包结构", "structure");
  if (id == "manifest-parse") return T(lang, "manifest 解析", "manifest parse");
  if (id == "manifest") return T(lang, "manifest 字段", "manifest fields");
  if (id == "entrypoint") return T(lang, "入口文件", "entrypoint");
  if (id == "elf-parse") return T(lang, "ELF 解析", "ELF parse");
  if (id == "arch") return T(lang, "目标架构", "target arch");
  if (id == "export-symbol") return T(lang, "导出符号", "exported symbols");
  if (id == "runpath") return T(lang, "运行库搜索路径", "runpath");
  if (id == "dependencies") return T(lang, "依赖项", "dependencies");
  if (id == "forbidden-symbols") return T(lang, "禁用符号", "forbidden symbols");
  if (id == "glibc") return T(lang, "glibc 版本", "glibc version");
  if (id == "libstdc++") return T(lang, "libstdc++ 版本", "libstdc++ version");
  if (id == "files") return T(lang, "文件清单", "files");
  if (id == "checksums") return T(lang, "校验和", "checksums");
  return "";
}

// ---- report aggregation ----
enum class Level { Pass, Warn, Fail };

struct Report {
  Lang lang = Lang::Zh;
  int fails = 0;
  int warns = 0;

  explicit Report(Lang l) : lang(l) {}

  void Add(Level lvl, const std::string& check, const std::string& detail) {
    const char* tag = lvl == Level::Pass ? "PASS" : (lvl == Level::Warn ? "WARN" : "FAIL");
    if (lvl == Level::Fail) ++fails;
    if (lvl == Level::Warn) ++warns;

    const char* name = CheckName(check, lang);
    std::string label = check;
    // Skip the parenthetical when it would just repeat the id (common in English).
    if (name[0] != '\0' && check != name) {
      // Chinese uses full-width parens, English uses ascii to read naturally.
      label += (lang == Lang::En) ? " (" : "（";
      label += name;
      label += (lang == Lang::En) ? ")" : "）";
    }
    if (detail.empty())
      std::printf("  [%s] %s\n", tag, label.c_str());
    else
      std::printf("  [%s] %s: %s\n", tag, label.c_str(), detail.c_str());
  }
};


// ---- individual check groups ----

void CheckElf(const std::string& entry, const WrapperManifest& m, Report& rep) {
  const Lang lang = rep.lang;
  ElfInfo info = InspectElf(entry);
  if (!info.ok) {
    rep.Add(Level::Fail, "elf-parse", info.error);
    return;
  }

  // arch
  if (!m.target_arch.empty()) {
    if (info.arch_name == m.target_arch)
      rep.Add(Level::Pass, "arch", info.arch_name);
    else
      rep.Add(Level::Fail, "arch",
              std::string(T(lang, "ELF 实际为 ", "ELF is ")) +
                  (info.arch_name.empty() ? T(lang, "未知", "unknown") : info.arch_name.c_str()) +
                  T(lang, "，manifest 声明为 ", ", manifest declares ") + m.target_arch);
  }

  // exported symbols: only sdk_vtable
  std::vector<std::string> leaks;
  bool has_vtable = false;
  for (const auto& s : info.exported) {
    if (s == "sdk_vtable") {
      has_vtable = true;
      continue;
    }
    if (!IsIgnoredExport(s)) leaks.push_back(s);
  }
  if (!has_vtable)
    rep.Add(Level::Fail, "export-symbol", T(lang, "未导出 sdk_vtable", "sdk_vtable not exported"));
  if (leaks.empty()) {
    if (has_vtable)
      rep.Add(Level::Pass, "export-symbol", T(lang, "仅导出 sdk_vtable", "only sdk_vtable exported"));
  } else {
    std::string detail = T(lang, "检测到 ", "found ") + std::to_string(leaks.size()) +
                         T(lang, " 个泄露符号，例如 ", " leaked symbol(s), e.g. ");
    for (std::size_t i = 0; i < leaks.size() && i < 5; ++i) {
      if (i) detail += ", ";
      detail += leaks[i];
    }
    rep.Add(Level::Fail, "export-symbol", detail);
  }

  // RUNPATH must contain $ORIGIN/lib as one of the colon-separated entries
  bool has_origin_lib = false;
  std::istringstream rp(info.runpath);
  std::string rp_entry;
  while (std::getline(rp, rp_entry, ':')) {
    if (rp_entry == "$ORIGIN/lib") {
      has_origin_lib = true;
      break;
    }
  }

  if (has_origin_lib) {
    rep.Add(Level::Pass, "runpath", info.runpath);
  } else if (!info.has_rpath && !info.has_runpath) {
    rep.Add(Level::Fail, "runpath",
            T(lang, "未设置 RPATH/RUNPATH（必须包含 $ORIGIN/lib）",
              "no RPATH/RUNPATH set (must contain $ORIGIN/lib)"));
  } else {
    rep.Add(Level::Fail, "runpath",
            T(lang, "当前为 '", "current is '") + info.runpath +
                T(lang, "'，必须包含 $ORIGIN/lib", "', must contain $ORIGIN/lib"));
  }

  // DT_NEEDED whitelist / private_libs / system_libs
  std::set<std::string> declared;
  for (const auto& pl : m.private_libs) declared.insert(BaseName(pl));
  // Host-provided libraries (e.g. ROS libs under /opt/ros for RMW wrappers)
  // declared in the manifest are allowed without bundling.
  for (const auto& sl : m.system_libs) declared.insert(BaseName(sl));
  std::vector<std::string> bad;
  for (const auto& need : info.needed) {
    if (IsSysLib(need)) continue;
    if (declared.count(need)) continue;
    bad.push_back(need);
  }
  if (bad.empty())
    rep.Add(Level::Pass, "dependencies",
            std::to_string(info.needed.size()) +
                T(lang, " 个 DT_NEEDED 依赖，全部在白名单/声明内",
                  " DT_NEEDED dependencies, all whitelisted/declared"));
  else {
    std::string detail = T(lang, "未声明的依赖: ", "undeclared dependencies: ");
    for (std::size_t i = 0; i < bad.size(); ++i) {
      if (i) detail += ", ";
      detail += bad[i];
    }
    rep.Add(Level::Fail, "dependencies", detail);
  }

  // forbidden imported symbols
  std::vector<std::string> forbidden_used;
  for (const auto& u : info.undefined) {
    if (IsForbidden(u)) forbidden_used.push_back(u);
  }
  if (forbidden_used.empty())
    rep.Add(Level::Pass, "forbidden-symbols",
            T(lang, "未引用任何禁用符号", "no forbidden symbols referenced"));
  else {
    std::string detail = T(lang, "引用了禁用符号: ", "references forbidden symbols: ");
    for (std::size_t i = 0; i < forbidden_used.size(); ++i) {
      if (i) detail += ", ";
      detail += forbidden_used[i];
    }
    rep.Add(Level::Fail, "forbidden-symbols", detail);
  }

  // glibc / libstdc++ version vs declared maxima
  if (!info.max_glibc.empty()) {
    if (!m.glibc_max.empty() && CompareVersion(info.max_glibc, m.glibc_max) > 0)
      rep.Add(Level::Fail, "glibc",
              T(lang, "需要 GLIBC_", "requires GLIBC_") + info.max_glibc +
                  T(lang, "，超过声明上限 ", ", exceeds declared max ") + m.glibc_max);
    else
      rep.Add(Level::Pass, "glibc", T(lang, "最高 GLIBC_", "max GLIBC_") + info.max_glibc);
  }
  if (!info.max_glibcxx.empty()) {
    if (!m.libstdcxx_max.empty()) {
      std::string declared_v = m.libstdcxx_max;
      const char* pfx = "GLIBCXX_";
      if (declared_v.compare(0, std::strlen(pfx), pfx) == 0)
        declared_v = declared_v.substr(std::strlen(pfx));
      if (CompareVersion(info.max_glibcxx, declared_v) > 0)
        rep.Add(Level::Fail, "libstdc++",
                T(lang, "需要 GLIBCXX_", "requires GLIBCXX_") + info.max_glibcxx +
                    T(lang, "，超过声明上限 ", ", exceeds declared max ") + m.libstdcxx_max);
      else
        rep.Add(Level::Pass, "libstdc++",
                T(lang, "最高 GLIBCXX_", "max GLIBCXX_") + info.max_glibcxx);
    } else {
      rep.Add(Level::Pass, "libstdc++",
              T(lang, "最高 GLIBCXX_", "max GLIBCXX_") + info.max_glibcxx);
    }
  }
}

void CheckFiles(const std::string& dir, const WrapperManifest& m, Report& rep) {
  const Lang lang = rep.lang;
  bool all_ok = true;
  for (const auto& f : m.files) {
    std::string full = JoinPath(dir, f.path);
    if (!FileExists(full)) {
      rep.Add(Level::Fail, "files", f.path + T(lang, " 未在包中找到", " not found in package"));
      all_ok = false;
      continue;
    }
    uint64_t sz = 0;
    if (f.size_set && FileSize(full, sz) && sz != f.size) {
      rep.Add(Level::Fail, "files",
              f.path + T(lang, " 实际大小 ", " actual size ") + std::to_string(sz) +
                  T(lang, " 字节，与 manifest 声明的 ", " bytes, differs from manifest's ") +
                  std::to_string(f.size) + T(lang, " 不一致", ""));
      all_ok = false;
    }
    bool ok = false;
    std::string digest = Sha256File(full, &ok);
    if (!ok) {
      rep.Add(Level::Fail, "files",
              f.path + T(lang, " 无法读取以计算哈希", " cannot be read to hash"));
      all_ok = false;
    } else if (digest != f.sha256) {
      rep.Add(Level::Fail, "files",
              f.path + T(lang, " 的 sha256 与 manifest 不一致", " sha256 differs from manifest"));
      all_ok = false;
    }
  }
  if (all_ok && !m.files.empty())
    rep.Add(Level::Pass, "files",
            std::to_string(m.files.size()) +
                T(lang, " 个文件与 manifest 一致", " files match manifest"));
}

void CheckChecksums(const std::string& dir, Report& rep) {
  const Lang lang = rep.lang;
  std::string cs = JoinPath(dir, "checksums.sha256");
  std::ifstream in(cs);
  if (!in) {
    return;  // presence is reported by CheckStructure
  }
  std::string line;
  int verified = 0;
  bool ok = true;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream ls(line);
    std::string hash, path;
    ls >> hash >> path;
    if (hash.empty() || path.empty()) continue;
    if (!path.empty() && path[0] == '*') path = path.substr(1);
    bool fok = false;
    std::string digest = Sha256File(JoinPath(dir, path), &fok);
    if (!fok || digest != hash) {
      rep.Add(Level::Fail, "checksums",
              T(lang, "校验失败或文件缺失: ", "verify failed or file missing: ") + path);
      ok = false;
    } else {
      ++verified;
    }
  }
  if (ok)
    rep.Add(Level::Pass, "checksums",
            T(lang, "已校验 ", "verified ") + std::to_string(verified) +
                T(lang, " 个文件", " file(s)"));
}

// Package layout rules (manual §5.1): manifest.yaml + checksums.sha256 present,
// the wrapper .so sits at the package root, private libraries live under lib/,
// and no other shared library is left at the root.
void CheckStructure(const std::string& dir, const WrapperManifest& m, Report& rep) {
  const Lang lang = rep.lang;
  if (FileExists(JoinPath(dir, "checksums.sha256")))
    rep.Add(Level::Pass, "structure", T(lang, "checksums.sha256 存在", "checksums.sha256 present"));
  else
    rep.Add(Level::Fail, "structure", T(lang, "缺少 checksums.sha256", "missing checksums.sha256"));

  if (!m.api_entrypoint.empty() && m.api_entrypoint.find('/') != std::string::npos)
    rep.Add(Level::Fail, "structure",
            T(lang, "wrapper .so 必须位于包根目录，而非子目录: ",
              "wrapper .so must be at package root, not a subdir: ") + m.api_entrypoint);

  for (const auto& pl : m.private_libs) {
    if (pl.compare(0, 4, "lib/") != 0)
      rep.Add(Level::Fail, "structure",
              T(lang, "私有库必须位于 lib/ 目录下: ", "private lib must live under lib/: ") + pl);
  }

  // Scan the package root for stray shared libraries.
  if (DIR* d = ::opendir(dir.c_str())) {
    int stray = 0;
    for (dirent* e = ::readdir(d); e; e = ::readdir(d)) {
      std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      if (!FileExists(JoinPath(dir, name))) continue;  // dirs/special skipped
      if (HasSoSuffix(name) && name != m.api_entrypoint) {
        rep.Add(Level::Fail, "structure",
                T(lang, "包根目录存在多余的共享库（请移到 lib/ 下）: ",
                  "stray shared lib at package root (move under lib/): ") + name);
        ++stray;
      }
    }
    ::closedir(d);
    if (stray == 0)
      rep.Add(Level::Pass, "structure", T(lang, "包目录结构正确", "package layout OK"));
  }
}

}  // namespace

int RunPackageCheck(const std::string& package_dir, Lang lang) {
  Report rep(lang);
  std::printf(T(lang, "=== 包检查: %s ===\n", "=== Package check: %s ===\n"),
              package_dir.c_str());

  // 1. package structure
  std::string manifest_path = JoinPath(package_dir, "manifest.yaml");
  if (!FileExists(manifest_path)) {
    rep.Add(Level::Fail, "structure", T(lang, "缺少 manifest.yaml", "missing manifest.yaml"));
    std::printf(T(lang, "\n结果: 失败（%d 项失败）\n", "\nResult: FAIL (%d failed)\n"), rep.fails);
    return 1;
  }
  rep.Add(Level::Pass, "structure", T(lang, "manifest.yaml 存在", "manifest.yaml present"));

  // 2. manifest parse + validate
  YamlParseResult yaml = ParseYamlFile(manifest_path);
  if (!yaml.ok) {
    rep.Add(Level::Fail, "manifest-parse", yaml.error);
    std::printf(T(lang, "\n结果: 失败（%d 项失败）\n", "\nResult: FAIL (%d failed)\n"), rep.fails);
    return 1;
  }
  WrapperManifest m;
  std::vector<std::string> merrs;
  ParseManifest(yaml.root, m, merrs);
  ValidateManifest(m, merrs);
  if (merrs.empty()) {
    rep.Add(Level::Pass, "manifest", T(lang, "字段校验通过", "field validation passed"));
  } else {
    for (const auto& e : merrs) rep.Add(Level::Fail, "manifest", e);
  }

  // 3. package layout
  CheckStructure(package_dir, m, rep);

  // 4. entrypoint
  std::string entry;
  if (!m.api_entrypoint.empty()) {
    entry = JoinPath(package_dir, m.api_entrypoint);
    if (!FileExists(entry)) {
      rep.Add(Level::Fail, "entrypoint", m.api_entrypoint + T(lang, " 未找到", " not found"));
      entry.clear();
    } else {
      rep.Add(Level::Pass, "entrypoint", m.api_entrypoint);
    }
  }

  // 5-7. ELF / files / checksums
  if (!entry.empty()) {
    CheckElf(entry, m, rep);
  }
  CheckFiles(package_dir, m, rep);
  CheckChecksums(package_dir, rep);

  if (lang == Lang::En) {
    std::printf("\nResult: %s (%d failed, %d warning(s))\n", rep.fails ? "FAIL" : "PASS", rep.fails,
                rep.warns);
  } else {
    std::printf("\n结果: %s（%d 项失败，%d 项警告）\n", rep.fails ? "失败" : "通过", rep.fails,
                rep.warns);
  }
  return rep.fails ? 1 : 0;
}

std::string ResolvePackageDir(const std::string& input, std::string& cleanup_dir, Lang lang) {
  cleanup_dir.clear();
  if (IsDir(input)) return input;
  if (!FileExists(input)) {
    std::fprintf(stderr, T(lang, "包: 路径不存在: %s\n", "package: path does not exist: %s\n"),
                 input.c_str());
    return {};
  }
  if (input.find('\'') != std::string::npos) {
    std::fprintf(stderr, "%s",
                 T(lang, "包: 压缩包路径包含单引号，请先手动解压\n",
                   "package: archive path contains a single quote, extract it manually first\n"));
    return {};
  }
  char tmpl[] = "/tmp/sdk_pkg_XXXXXX";
  char* d = ::mkdtemp(tmpl);
  if (!d) {
    std::fprintf(stderr, "%s", T(lang, "包: mkdtemp 失败\n", "package: mkdtemp failed\n"));
    return {};
  }
  cleanup_dir = d;
  std::string cmd = EndsWith(input, ".zip")
                        ? "unzip -q -o '" + input + "' -d '" + cleanup_dir + "'"
                        : "tar -xf '" + input + "' -C '" + cleanup_dir + "'";  // tar detects gz
  if (std::system(cmd.c_str()) != 0) {
    std::fprintf(stderr, "%s",
                 T(lang, "包: 解压失败（需要 tar/unzip）；请手动解压后传入目录\n",
                   "package: extraction failed (needs tar/unzip); extract manually and pass the "
                   "directory\n"));
    return {};
  }
  if (FileExists(JoinPath(cleanup_dir, "manifest.yaml"))) return cleanup_dir;
  std::fprintf(stderr, "%s",
               T(lang, "包: 压缩包中未找到 manifest.yaml\n",
                 "package: manifest.yaml not found in archive\n"));
  return {};
}

}  // namespace sdk_diag
