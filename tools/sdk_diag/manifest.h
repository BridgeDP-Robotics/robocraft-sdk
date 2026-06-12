// manifest — POD model of wrapper manifest.yaml plus hand-written parse and
// validation (no JSON/reflection library, to keep the tool dependency-free).
// The JSON Schema shipped alongside (wrapper-manifest-v1.schema.json) is the
// authoritative field reference; this mirrors it.
#ifndef SDK_DIAG_MANIFEST_H
#define SDK_DIAG_MANIFEST_H

#include <cstdint>
#include <string>
#include <vector>

#include "yaml_lite.h"

namespace sdk_diag {

struct ManifestFile {
  std::string path;
  std::string sha256;
  uint64_t size = 0;
  bool size_set = false;
};

struct WrapperManifest {
  int schema_version = 0;

  std::string sdk_name;
  std::string sdk_version;
  std::string sdk_description;

  std::string vendor_name;
  std::string vendor_contact;
  std::string vendor_license;

  std::string robot_model;
  std::string robot_firmware_min;

  std::string target_arch;
  std::string target_os;
  std::string glibc_max;
  std::string libstdcxx_max;

  std::string api_version;
  std::string api_entrypoint;
  std::vector<std::string> capabilities;

  std::string net_interface;
  std::string net_protocol;
  std::vector<std::string> net_ports;

  std::vector<ManifestFile> files;
  std::vector<std::string> private_libs;
  // Host-provided shared libraries (beyond the built-in system whitelist) that
  // the wrapper is allowed to depend on without bundling — e.g. ROS libraries
  // resolved from /opt/ros/<distro>/lib for RMW wrappers. Declared by base name.
  std::vector<std::string> system_libs;
};

// Reads fields out of a parsed YAML tree. Structural problems (wrong node
// type for a known field) are appended to `errors`. Returns false only when
// the root is not a mapping.
bool ParseManifest(const YamlNode& root, WrapperManifest& out, std::vector<std::string>& errors);

// Semantic validation: required fields present, enums valid, etc. Appends to
// `errors`. Returns true when no errors were added.
bool ValidateManifest(const WrapperManifest& m, std::vector<std::string>& errors);

}  // namespace sdk_diag

#endif  // SDK_DIAG_MANIFEST_H
