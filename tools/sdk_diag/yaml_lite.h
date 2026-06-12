// yaml_lite — a minimal YAML subset parser, just enough for wrapper manifests.
//
// Supported: indentation-based block mappings and sequences, scalar values
// (bare / single- / double-quoted), comments (# ...), nested maps, and
// sequences of scalars or of maps (e.g. files: - path: ...). This is NOT a
// general YAML implementation; it deliberately covers only what the manifest
// schema needs so the tool stays dependency-free.
#ifndef SDK_DIAG_YAML_LITE_H
#define SDK_DIAG_YAML_LITE_H

#include <string>
#include <utility>
#include <vector>

namespace sdk_diag {

struct YamlNode {
  enum class Type { Null, Scalar, Map, Sequence };
  Type type = Type::Null;

  std::string scalar;
  std::vector<std::pair<std::string, YamlNode>> map;  // insertion order preserved
  std::vector<YamlNode> seq;

  bool IsNull() const { return type == Type::Null; }
  bool IsScalar() const { return type == Type::Scalar; }
  bool IsMap() const { return type == Type::Map; }
  bool IsSequence() const { return type == Type::Sequence; }

  // Returns nullptr when this is not a map or the key is absent.
  const YamlNode* Find(const std::string& key) const;

  // Convenience: scalar value or empty string.
  const std::string& Str() const;
};

struct YamlParseResult {
  bool ok = false;
  std::string error;  // human-readable, includes 1-based line number on failure
  YamlNode root;
};

YamlParseResult ParseYaml(const std::string& text);

// Reads a file and parses it. On file-open failure, ok is false.
YamlParseResult ParseYamlFile(const std::string& path);

}  // namespace sdk_diag

#endif  // SDK_DIAG_YAML_LITE_H
