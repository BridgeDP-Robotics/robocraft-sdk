// package_check — orchestrates the static + load checks documented for
// `sdk_diag --check`. Depends only on the public interface header and the
// sibling self-contained modules (elf_inspect / manifest / sha256 / yaml_lite).
#ifndef SDK_DIAG_PACKAGE_CHECK_H
#define SDK_DIAG_PACKAGE_CHECK_H

#include <string>

namespace sdk_diag {

// Output language for the --check report. Default is Chinese.
enum class Lang { Zh, En };

// Runs all checks on an already-extracted package directory and prints a
// report. Returns 0 when every mandatory check passed, non-zero otherwise.
// The report text is rendered in `lang` (defaults to Chinese); the English
// check id always follows the [PASS]/[WARN]/[FAIL] tag so CI greps stay stable.
int RunPackageCheck(const std::string& package_dir, Lang lang = Lang::Zh);

// Resolves a package input (directory or archive) to a directory containing
// manifest.yaml. Archives (.tar[.gz]/.tgz/.zip) are extracted into a temp dir
// via system tar/unzip; on success cleanup_dir names a temp dir the caller
// should rm -rf afterwards. Returns "" on failure (reason printed to stderr).
std::string ResolvePackageDir(const std::string& input, std::string& cleanup_dir,
                              Lang lang = Lang::Zh);

}  // namespace sdk_diag

#endif  // SDK_DIAG_PACKAGE_CHECK_H
