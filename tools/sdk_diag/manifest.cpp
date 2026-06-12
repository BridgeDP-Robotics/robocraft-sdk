#include "manifest.h"

#include <array>
#include <cctype>
#include <cstdlib>

namespace sdk_diag {
namespace {

std::string Scalar(const YamlNode* n) {
  return (n && n->IsScalar()) ? n->scalar : std::string();
}

std::string Scalar(const YamlNode& parent, const char* key) {
  return Scalar(parent.Find(key));
}

void ReadStringSeq(const YamlNode* n, std::vector<std::string>& out) {
  if (!n || !n->IsSequence()) return;
  for (const auto& item : n->seq) {
    if (item.IsScalar()) out.push_back(item.scalar);
  }
}

bool IsHex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

bool LooksLikeGpl(const std::string& lic) {
  // Reject plain GPL (LGPL is fine). Case-insensitive contains "GPL" but not "LGPL".
  std::string u;
  for (char c : lic) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  bool has_gpl = u.find("GPL") != std::string::npos;
  bool has_lgpl = u.find("LGPL") != std::string::npos;
  return has_gpl && !has_lgpl;
}

const std::array<const char*, 12> kKnownCaps = {
    "SDK_CAP_LOW_LEVEL", "SDK_CAP_IMU",         "SDK_CAP_BATTERY",   "SDK_CAP_FOOT_SENSOR",
    "SDK_CAP_ODOMETRY",  "SDK_CAP_HAND_LEFT",   "SDK_CAP_HAND_RIGHT", "SDK_CAP_JOYSTICK",
    "SDK_CAP_FALL_DETECT", "SDK_CAP_MOTOR_TEMP", "SDK_CAP_MOTOR_MODE", "SDK_CAP_NATIVE_QUAT"};

bool IsKnownCap(const std::string& c) {
  for (const char* k : kKnownCaps) {
    if (c == k) return true;
  }
  return false;
}

}  // namespace

bool ParseManifest(const YamlNode& root, WrapperManifest& m, std::vector<std::string>& errors) {
  if (!root.IsMap()) {
    errors.push_back("manifest root is not a mapping");
    return false;
  }

  if (const YamlNode* sv = root.Find("schema_version")) {
    if (sv->IsScalar()) m.schema_version = std::atoi(sv->scalar.c_str());
  }

  if (const YamlNode* sdk = root.Find("sdk"); sdk && sdk->IsMap()) {
    m.sdk_name = Scalar(*sdk, "name");
    m.sdk_version = Scalar(*sdk, "version");
    m.sdk_description = Scalar(*sdk, "description");
  }
  if (const YamlNode* v = root.Find("vendor"); v && v->IsMap()) {
    m.vendor_name = Scalar(*v, "name");
    m.vendor_contact = Scalar(*v, "contact");
    m.vendor_license = Scalar(*v, "license");
  }
  if (const YamlNode* r = root.Find("robot"); r && r->IsMap()) {
    m.robot_model = Scalar(*r, "model");
    m.robot_firmware_min = Scalar(*r, "firmware_version_min");
  }
  if (const YamlNode* t = root.Find("target"); t && t->IsMap()) {
    m.target_arch = Scalar(*t, "arch");
    m.target_os = Scalar(*t, "os");
    m.glibc_max = Scalar(*t, "glibc_max");
    m.libstdcxx_max = Scalar(*t, "libstdcxx_max");
  }
  if (const YamlNode* a = root.Find("api"); a && a->IsMap()) {
    m.api_version = Scalar(*a, "version");
    m.api_entrypoint = Scalar(*a, "entrypoint");
    ReadStringSeq(a->Find("capabilities"), m.capabilities);
  }
  if (const YamlNode* net = root.Find("network"); net && net->IsMap()) {
    m.net_interface = Scalar(*net, "interface");
    m.net_protocol = Scalar(*net, "protocol");
    ReadStringSeq(net->Find("ports"), m.net_ports);
  }
  ReadStringSeq(root.Find("private_libs"), m.private_libs);
  ReadStringSeq(root.Find("system_libs"), m.system_libs);

  if (const YamlNode* files = root.Find("files"); files && files->IsSequence()) {
    for (const auto& item : files->seq) {
      if (!item.IsMap()) {
        errors.push_back("files[] entry is not a mapping");
        continue;
      }
      ManifestFile f;
      f.path = Scalar(item, "path");
      f.sha256 = Scalar(item, "sha256");
      std::string size_str = Scalar(item, "size");
      if (!size_str.empty()) {
        f.size = std::strtoull(size_str.c_str(), nullptr, 10);
        f.size_set = true;
      }
      m.files.push_back(std::move(f));
    }
  }
  return true;
}

bool ValidateManifest(const WrapperManifest& m, std::vector<std::string>& errors) {
  std::size_t before = errors.size();
  auto require = [&](const std::string& val, const char* field) {
    if (val.empty()) errors.push_back(std::string("missing required field: ") + field);
  };

  if (m.schema_version != 1)
    errors.push_back("schema_version must be 1");

  require(m.sdk_name, "sdk.name");
  require(m.sdk_version, "sdk.version");
  require(m.vendor_name, "vendor.name");
  require(m.vendor_contact, "vendor.contact");
  require(m.vendor_license, "vendor.license");
  if (!m.vendor_license.empty() && LooksLikeGpl(m.vendor_license))
    errors.push_back("vendor.license: GPL (non-LGPL) is not allowed");

  require(m.robot_model, "robot.model");
  require(m.robot_firmware_min, "robot.firmware_version_min");

  require(m.target_arch, "target.arch");
  if (!m.target_arch.empty() && m.target_arch != "x86_64" && m.target_arch != "aarch64")
    errors.push_back("target.arch must be x86_64 or aarch64 (got: " + m.target_arch + ")");

  require(m.api_version, "api.version");
  require(m.api_entrypoint, "api.entrypoint");

  for (const auto& c : m.capabilities) {
    if (!IsKnownCap(c)) errors.push_back("unknown capability: " + c);
  }

  // network.interface is intentionally not required: the runtime NIC is chosen
  // at runtime via config.json ("network_interface"), not baked into the manifest.
  require(m.net_protocol, "network.protocol");

  if (m.files.empty()) {
    errors.push_back("files[] must list at least the entrypoint");
  }
  for (const auto& f : m.files) {
    if (f.path.empty()) errors.push_back("files[] entry missing path");
    if (!IsHex64(f.sha256))
      errors.push_back("files[" + f.path + "] sha256 must be 64 hex chars");
    if (!f.size_set) errors.push_back("files[" + f.path + "] missing size");
  }

  return errors.size() == before;
}

}  // namespace sdk_diag
