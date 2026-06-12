/**
 * @file unitree_g1_wrapper.cpp
 * @brief Unitree G1 vtable-based SDK wrapper
 *
 * Exports a single symbol: SDK_API extern const sdk_vtable_t sdk_vtable.
 * All Unitree SDK2 resources are owned by an opaque SdkHandleImpl.
 */

#include <unitree/idl/hg/LowCmd_.hpp>
#include <unitree/idl/hg/LowState_.hpp>
#include <unitree/robot/b2/motion_switcher/motion_switcher_client.hpp>
#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_publisher.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>
#include <unitree/robot/g1/loco/g1_loco_client.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "sdk_wrapper/interface/sdk_wrapper_interface.h"

using namespace unitree::robot;
using namespace unitree_hg::msg::dds_;

// ============================================================================
// Constants
// ============================================================================

static constexpr int kG1NumMotors = 29;
static constexpr int kJoyAxes = 4;
static constexpr int kJoyButtons = 12;
static constexpr uint32_t kCapabilities =
    SDK_CAP_LOW_LEVEL | SDK_CAP_IMU | SDK_CAP_JOYSTICK | SDK_CAP_NATIVE_QUAT;

// ============================================================================
// Joystick Raw Data Layout
// ============================================================================
// From unitree_sdk2/example/g1/low_level/gamepad.hpp

#pragma pack(push, 1)
typedef union {
  struct {
    uint8_t R1 : 1;
    uint8_t L1 : 1;
    uint8_t start : 1;
    uint8_t select : 1;
    uint8_t R2 : 1;
    uint8_t L2 : 1;
    uint8_t F1 : 1;
    uint8_t F2 : 1;
    uint8_t A : 1;
    uint8_t B : 1;
    uint8_t X : 1;
    uint8_t Y : 1;
    uint8_t up : 1;
    uint8_t right : 1;
    uint8_t down : 1;
    uint8_t left : 1;
  } components;
  uint16_t value;
} G1KeySwitch;

typedef struct {
  uint8_t head[2];
  G1KeySwitch btn;
  float lx;
  float rx;
  float ry;
  float L2;
  float ly;
  uint8_t idle[16];
} G1RemoteData;
#pragma pack(pop)

// ============================================================================
// CRC32
// ============================================================================
// From unitree_sdk2/example/g1/low_level/g1_ankle_swing_example.cpp

static uint32_t Crc32Core(uint32_t* ptr, uint32_t len) {
  uint32_t xbit = 0;
  uint32_t data = 0;
  uint32_t CRC32 = 0xFFFFFFFF;
  const uint32_t dwPolynomial = 0x04c11db7;
  for (uint32_t i = 0; i < len; i++) {
    xbit = 1 << 31;
    data = ptr[i];
    for (uint32_t bits = 0; bits < 32; bits++) {
      if (CRC32 & 0x80000000) {
        CRC32 <<= 1;
        CRC32 ^= dwPolynomial;
      } else {
        CRC32 <<= 1;
      }
      if (data & xbit) {
        CRC32 ^= dwPolynomial;
      }
      xbit >>= 1;
    }
  }
  return CRC32;
}

// ============================================================================
// Helpers and internal state
// ============================================================================

enum class PrAbMode : uint8_t {
  kPR = 0,  // Series Control for Pitch/Roll Joints
  kAB = 1   // Parallel Control for A/B Joints
};

static int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct StateSnapshot {
  float motor_q[kG1NumMotors]{};
  float motor_dq[kG1NumMotors]{};
  float motor_tau[kG1NumMotors]{};
  float imu_quat[4]{1.0f, 0.0f, 0.0f, 0.0f};
  float imu_rpy[3]{};
  float imu_gyro[3]{};
  float imu_acc[3]{};
  float joy_axes[kJoyAxes]{};
  bool joy_buttons[kJoyButtons]{};
  uint8_t mode_machine{0};
  uint8_t mode_pr{static_cast<uint8_t>(PrAbMode::kPR)};
};

struct SdkHandleImpl {
  std::shared_ptr<ChannelSubscriber<LowState_>> state_sub;
  std::shared_ptr<ChannelPublisher<LowCmd_>> cmd_pub;
  std::shared_ptr<unitree::robot::b2::MotionSwitcherClient> motion_switcher;
  std::shared_ptr<unitree::robot::g1::LocoClient> loco_client;

  std::mutex write_mutex;
  StateSnapshot write_buf;
  StateSnapshot read_buf;
  std::atomic<bool> new_data{false};

  std::atomic<uint64_t> state_cb_count{0};
  std::atomic<uint64_t> update_count{0};
  std::atomic<uint64_t> cmd_count{0};
  std::atomic<int64_t> last_state_cb_us{0};
  std::atomic<int64_t> last_joy_cb_us{0};
  std::atomic<uint8_t> latest_mode_machine{0};
  std::atomic<uint8_t> latest_mode_pr{static_cast<uint8_t>(PrAbMode::kPR)};

  float cmd_q[kG1NumMotors]{};
  float cmd_dq[kG1NumMotors]{};
  float cmd_tau[kG1NumMotors]{};
  float cmd_kp[kG1NumMotors]{};
  float cmd_kd[kG1NumMotors]{};

  // Config-derived values (set during VCreate)
  int max_release_retries{5};
  std::string unready_mode_alias{"ai"};
  int retry_interval_s{3};
};

struct sdk_handle_s : SdkHandleImpl {};

static SdkHandleImpl* Cast(sdk_handle_t h) {
  return static_cast<SdkHandleImpl*>(h);
}

static void ParseJoystick(const std::array<uint8_t, 40>& remote, StateSnapshot* out) {
  if (!out) {
    return;
  }

  G1RemoteData raw{};
  std::memcpy(&raw, remote.data(), sizeof(G1RemoteData));

  out->joy_axes[SDK_JOY_AXIS_LX] = raw.lx;
  out->joy_axes[SDK_JOY_AXIS_LY] = raw.ly;
  out->joy_axes[SDK_JOY_AXIS_RX] = raw.rx;
  out->joy_axes[SDK_JOY_AXIS_RY] = raw.ry;

  out->joy_buttons[SDK_JOY_BTN_A] = raw.btn.components.A;
  out->joy_buttons[SDK_JOY_BTN_B] = raw.btn.components.B;
  out->joy_buttons[SDK_JOY_BTN_X] = raw.btn.components.X;
  out->joy_buttons[SDK_JOY_BTN_Y] = raw.btn.components.Y;
  out->joy_buttons[SDK_JOY_BTN_LB] = raw.btn.components.L1;
  out->joy_buttons[SDK_JOY_BTN_RB] = raw.btn.components.R1;
  out->joy_buttons[SDK_JOY_BTN_BACK] = raw.btn.components.select;
  out->joy_buttons[SDK_JOY_BTN_START] = raw.btn.components.start;
  out->joy_buttons[SDK_JOY_BTN_LT] = raw.btn.components.L2;
  out->joy_buttons[SDK_JOY_BTN_RT] = raw.btn.components.R2;
  out->joy_buttons[SDK_JOY_BTN_DPAD_UP] = raw.btn.components.up;
  out->joy_buttons[SDK_JOY_BTN_DPAD_DOWN] = raw.btn.components.down;
  out->joy_buttons[SDK_JOY_BTN_DPAD_LEFT] = raw.btn.components.left;
  out->joy_buttons[SDK_JOY_BTN_DPAD_RIGHT] = raw.btn.components.right;
}

static bool VerifyLowStateCrc(const LowState_& state) {
  auto copy = state;
  uint32_t expected = copy.crc();
  uint32_t computed =
      Crc32Core(reinterpret_cast<uint32_t*>(&copy), (sizeof(LowState_) >> 2) - 1);
  return expected == computed;
}

static void HandleLowState(SdkHandleImpl* impl, const LowState_& state) {
  if (!impl) {
    return;
  }

  std::lock_guard<std::mutex> lock(impl->write_mutex);
  for (int i = 0; i < kG1NumMotors; i++) {
    impl->write_buf.motor_q[i] = state.motor_state()[i].q();
    impl->write_buf.motor_dq[i] = state.motor_state()[i].dq();
    impl->write_buf.motor_tau[i] = state.motor_state()[i].tau_est();
  }

  const auto& imu = state.imu_state();
  for (int i = 0; i < 3; i++) {
    impl->write_buf.imu_rpy[i] = imu.rpy()[i];
    impl->write_buf.imu_gyro[i] = imu.gyroscope()[i];
    impl->write_buf.imu_acc[i] = imu.accelerometer()[i];
  }
  for (int i = 0; i < 4; i++) {
    impl->write_buf.imu_quat[i] = imu.quaternion()[i];
  }

  impl->write_buf.mode_machine = state.mode_machine();
  impl->write_buf.mode_pr = state.mode_pr();
  ParseJoystick(state.wireless_remote(), &impl->write_buf);

  impl->latest_mode_machine.store(state.mode_machine(), std::memory_order_relaxed);
  impl->latest_mode_pr.store(state.mode_pr(), std::memory_order_relaxed);
  impl->new_data.store(true, std::memory_order_release);
  impl->state_cb_count.fetch_add(1, std::memory_order_relaxed);
  impl->last_state_cb_us.store(NowUs(), std::memory_order_relaxed);
  impl->last_joy_cb_us.store(NowUs(), std::memory_order_relaxed);
}

// ============================================================================
// Simple JSON helpers (no external dependency)
// ============================================================================

static std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static std::string JsonStr(const std::string& json, const std::string& key,
                           const std::string& default_val = "") {
  std::string search = "\"" + key + "\"";
  auto pos = json.find(search);
  if (pos == std::string::npos) return default_val;
  pos = json.find(':', pos + search.size());
  if (pos == std::string::npos) return default_val;
  pos++;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' ||
                                json[pos] == '\r'))
    pos++;
  if (pos >= json.size()) return default_val;
  if (json[pos] == '"') {
    pos++;
    std::string out;
    bool escape = false;
    while (pos < json.size()) {
      if (escape) {
        out += json[pos];
        escape = false;
        pos++;
        continue;
      }
      if (json[pos] == '\\') {
        escape = true;
        pos++;
        continue;
      }
      if (json[pos] == '"') break;
      out += json[pos];
      pos++;
    }
    return out;
  }
  size_t end = pos;
  while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ' ' &&
         json[end] != '\t' && json[end] != '\n' && json[end] != '\r')
    end++;
  return json.substr(pos, end - pos);
}

static int64_t JsonInt(const std::string& json, const std::string& key, int64_t default_val = 0) {
  auto val = JsonStr(json, key);
  if (val.empty()) return default_val;
  return std::stoll(val);
}

static double JsonDouble(const std::string& json, const std::string& key,
                         double default_val = 0.0) {
  auto val = JsonStr(json, key);
  if (val.empty()) return default_val;
  return std::stod(val);
}

// ============================================================================
// vtable function implementations
// ============================================================================

static int VCreate(const char* wrapper_dir, sdk_handle_t* out) {
  if (!out) return SDK_ERR_INVALID_ARG;
  *out = nullptr;

  const std::string config_path = std::string(wrapper_dir) + "/config.json";
  std::fprintf(stderr, "unitree_g1_wrapper: VCreate begin (wrapper_dir=%s, config=%s)\n",
               wrapper_dir ? wrapper_dir : "(null)", config_path.c_str());
  std::string config_json = ReadFile(config_path);
  int domain_id = static_cast<int>(JsonInt(config_json, "domain_id", 0));
  std::string net_if = JsonStr(config_json, "network_interface", "");
  std::string topic_low_state = JsonStr(config_json, "topic_low_state", "rt/lowstate");
  std::string topic_low_cmd = JsonStr(config_json, "topic_low_cmd", "rt/lowcmd");
  int max_release_retries = static_cast<int>(JsonInt(config_json, "max_release_retries", 5));
  std::string unready_mode_alias = JsonStr(config_json, "unready_mode_alias", "ai");
  float motion_switcher_timeout = static_cast<float>(JsonDouble(config_json, "motion_switcher_timeout_s", 5.0));
  int retry_interval_s = static_cast<int>(JsonInt(config_json, "retry_interval_s", 3));
  std::fprintf(stderr,
               "unitree_g1_wrapper: config loaded (bytes=%zu, domain=%d, net_if=%s, "
               "low_state=%s, low_cmd=%s)\n",
               config_json.size(), domain_id, net_if.c_str(), topic_low_state.c_str(),
               topic_low_cmd.c_str());

  try {
    std::fprintf(stderr, "unitree_g1_wrapper: ChannelFactory::Init begin\n");
    ChannelFactory::Instance()->Init(domain_id, net_if.c_str());
    std::fprintf(stderr, "unitree_g1_wrapper: ChannelFactory::Init ok\n");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "unitree_g1_wrapper: ChannelFactory::Init failed: %s\n", e.what());
    return SDK_ERR_INTERNAL;
  }

  auto* h = new SdkHandleImpl();
  h->max_release_retries = max_release_retries;
  h->unready_mode_alias = unready_mode_alias;
  h->retry_interval_s = retry_interval_s;

  try {
    std::fprintf(stderr, "unitree_g1_wrapper: lowstate subscriber init begin\n");
    h->state_sub = std::make_shared<ChannelSubscriber<LowState_>>(topic_low_state);
    h->state_sub->InitChannel(
        [h](const void* msg) {
          const auto& state = *static_cast<const LowState_*>(msg);
          if (!VerifyLowStateCrc(state)) {
            return;
          }
          HandleLowState(h, state);
        },
        1);
    std::fprintf(stderr, "unitree_g1_wrapper: lowstate subscriber init ok\n");

    std::fprintf(stderr, "unitree_g1_wrapper: lowcmd publisher init begin\n");
    h->cmd_pub = std::make_shared<ChannelPublisher<LowCmd_>>(topic_low_cmd);
    h->cmd_pub->InitChannel();
    std::fprintf(stderr, "unitree_g1_wrapper: lowcmd publisher init ok\n");

    try {
      std::fprintf(stderr, "unitree_g1_wrapper: MotionSwitcherClient init begin\n");
      h->motion_switcher = std::make_shared<unitree::robot::b2::MotionSwitcherClient>();
      h->motion_switcher->SetTimeout(motion_switcher_timeout);
      h->motion_switcher->Init();
      std::fprintf(stderr, "unitree_g1_wrapper: MotionSwitcherClient init ok\n");
    } catch (const std::exception& ce) {
      std::fprintf(stderr, "unitree_g1_wrapper: MotionSwitcherClient init failed: %s\n", ce.what());
      h->motion_switcher.reset();
    }

    try {
      std::fprintf(stderr, "unitree_g1_wrapper: LocoClient init begin\n");
      h->loco_client = std::make_shared<unitree::robot::g1::LocoClient>();
      h->loco_client->SetTimeout(motion_switcher_timeout);
      h->loco_client->Init();
      std::fprintf(stderr, "unitree_g1_wrapper: LocoClient init ok\n");
    } catch (const std::exception& ce) {
      std::fprintf(stderr, "unitree_g1_wrapper: LocoClient init failed: %s\n", ce.what());
      h->loco_client.reset();
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "unitree_g1_wrapper: DDS channel init failed: %s\n", e.what());
    delete h;
    return SDK_ERR_INTERNAL;
  }

  *out = static_cast<sdk_handle_t>(h);
  std::fprintf(stderr, "unitree_g1_wrapper: VCreate ok\n");
  return SDK_OK;
}

static void VDestroy(sdk_handle_t handle) {
  delete Cast(handle);
}

static int VGetNumMotors(sdk_handle_t /*h*/) {
  return kG1NumMotors;
}

static uint32_t VGetCapabilities(sdk_handle_t /*h*/) {
  return kCapabilities;
}

static const char* VGetVersion(sdk_handle_t /*h*/) {
  return "1.0.0-g1-vtable";
}

static int VUpdate(sdk_handle_t h) {
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  impl->update_count.fetch_add(1, std::memory_order_relaxed);
  if (impl->new_data.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(impl->write_mutex);
    impl->read_buf = impl->write_buf;
    impl->new_data.store(false, std::memory_order_release);
  }
  return SDK_OK;
}

static float VGetMotorQ(sdk_handle_t h, int idx) {
  auto* impl = Cast(h);
  if (!impl || idx < 0 || idx >= kG1NumMotors) {
    return 0.0f;
  }
  return impl->read_buf.motor_q[idx];
}

static float VGetMotorDq(sdk_handle_t h, int idx) {
  auto* impl = Cast(h);
  if (!impl || idx < 0 || idx >= kG1NumMotors) {
    return 0.0f;
  }
  return impl->read_buf.motor_dq[idx];
}

static float VGetMotorTau(sdk_handle_t h, int idx) {
  auto* impl = Cast(h);
  if (!impl || idx < 0 || idx >= kG1NumMotors) {
    return 0.0f;
  }
  return impl->read_buf.motor_tau[idx];
}

static int VGetMotorQBatch(sdk_handle_t h, float* out, int max) {
  if (!out) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  if (max <= 0) {
    return 0;
  }
  int n = std::min(max, kG1NumMotors);
  std::memcpy(out, impl->read_buf.motor_q, static_cast<size_t>(n) * sizeof(float));
  return n;
}

static int VGetMotorDqBatch(sdk_handle_t h, float* out, int max) {
  if (!out) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  if (max <= 0) {
    return 0;
  }
  int n = std::min(max, kG1NumMotors);
  std::memcpy(out, impl->read_buf.motor_dq, static_cast<size_t>(n) * sizeof(float));
  return n;
}

static int VGetMotorTauBatch(sdk_handle_t h, float* out, int max) {
  if (!out) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  if (max <= 0) {
    return 0;
  }
  int n = std::min(max, kG1NumMotors);
  std::memcpy(out, impl->read_buf.motor_tau, static_cast<size_t>(n) * sizeof(float));
  return n;
}

static void VBeginCmd(sdk_handle_t h) {
  auto* impl = Cast(h);
  if (!impl) {
    return;
  }
  std::memset(impl->cmd_q, 0, sizeof(impl->cmd_q));
  std::memset(impl->cmd_dq, 0, sizeof(impl->cmd_dq));
  std::memset(impl->cmd_tau, 0, sizeof(impl->cmd_tau));
  std::memset(impl->cmd_kp, 0, sizeof(impl->cmd_kp));
  std::memset(impl->cmd_kd, 0, sizeof(impl->cmd_kd));
}

static void VSetMotorCmd(sdk_handle_t h, int idx, float q, float dq, float tau, float kp,
                         float kd) {
  auto* impl = Cast(h);
  if (!impl || idx < 0 || idx >= kG1NumMotors) {
    return;
  }
  impl->cmd_q[idx] = q;
  impl->cmd_dq[idx] = dq;
  impl->cmd_tau[idx] = tau;
  impl->cmd_kp[idx] = kp;
  impl->cmd_kd[idx] = kd;
}

static void VCommitCmd(sdk_handle_t h) {
  auto* impl = Cast(h);
  if (!impl || !impl->cmd_pub) {
    return;
  }

  LowCmd_ dds_cmd;
  dds_cmd.mode_pr() = impl->latest_mode_pr.load(std::memory_order_relaxed);
  dds_cmd.mode_machine() = impl->latest_mode_machine.load(std::memory_order_relaxed);

  for (int i = 0; i < kG1NumMotors; i++) {
    auto& m = dds_cmd.motor_cmd()[i];
    m.mode() = 1;
    m.q() = impl->cmd_q[i];
    m.dq() = impl->cmd_dq[i];
    m.tau() = impl->cmd_tau[i];
    m.kp() = impl->cmd_kp[i];
    m.kd() = impl->cmd_kd[i];
  }

  dds_cmd.crc() = Crc32Core(reinterpret_cast<uint32_t*>(&dds_cmd), (sizeof(LowCmd_) >> 2) - 1);
  impl->cmd_pub->Write(dds_cmd);
  impl->cmd_count.fetch_add(1, std::memory_order_relaxed);
}

static bool VHasImu(sdk_handle_t /*h*/) {
  return true;
}

static bool VHasJoystick(sdk_handle_t /*h*/) {
  return true;
}

static bool VHasBattery(sdk_handle_t /*h*/) {
  return false;
}

static bool VHasFootSensor(sdk_handle_t /*h*/) {
  return false;
}

static bool VHasOdometry(sdk_handle_t /*h*/) {
  return false;
}

static bool VHasHand(sdk_handle_t /*h*/, int /*side*/) {
  return false;
}

static int VGetImuQuat(sdk_handle_t h, float quat[4]) {
  if (!quat) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  std::memcpy(quat, impl->read_buf.imu_quat, 4 * sizeof(float));
  return SDK_OK;
}

static int VGetImuGyro(sdk_handle_t h, float gyro[3]) {
  if (!gyro) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  std::memcpy(gyro, impl->read_buf.imu_gyro, 3 * sizeof(float));
  return SDK_OK;
}

static int VGetImuRpy(sdk_handle_t h, float rpy[3]) {
  if (!rpy) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  std::memcpy(rpy, impl->read_buf.imu_rpy, 3 * sizeof(float));
  return SDK_OK;
}

static int VGetImuAcc(sdk_handle_t h, float acc[3]) {
  if (!acc) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  std::memcpy(acc, impl->read_buf.imu_acc, 3 * sizeof(float));
  return SDK_OK;
}

static float VGetBatteryVoltage(sdk_handle_t /*h*/) {
  return 0.0f;
}

static float VGetBatteryCurrent(sdk_handle_t /*h*/) {
  return 0.0f;
}

static float VGetBatteryPercentage(sdk_handle_t /*h*/) {
  return 0.0f;
}

static int VGetFootForce(sdk_handle_t /*h*/, int /*foot*/, float* /*forces*/,
                         int /*max_channels*/) {
  return SDK_ERR_NOT_SUPPORTED;
}

static int VGetFootContact(sdk_handle_t /*h*/, int /*foot*/, bool* /*contacts*/,
                           int /*max_channels*/) {
  return SDK_ERR_NOT_SUPPORTED;
}

static int VGetOdomPos(sdk_handle_t /*h*/, double /*pos*/[3]) {
  return SDK_ERR_NOT_SUPPORTED;
}

static int VGetOdomVel(sdk_handle_t /*h*/, double /*vel*/[3]) {
  return SDK_ERR_NOT_SUPPORTED;
}

static int VGetHandJointPos(sdk_handle_t /*h*/, int /*side*/, float* /*pos*/,
                            int /*max_joints*/) {
  return SDK_ERR_NOT_SUPPORTED;
}

static int VGetJoyAxes(sdk_handle_t h, float* axes, int max_axes) {
  if (!axes) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  if (max_axes <= 0) {
    return 0;
  }
  int n = std::min(max_axes, kJoyAxes);
  std::memcpy(axes, impl->read_buf.joy_axes, static_cast<size_t>(n) * sizeof(float));
  return n;
}

static int VGetJoyButtons(sdk_handle_t h, bool* buttons, int max_buttons) {
  if (!buttons) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  if (max_buttons <= 0) {
    return 0;
  }
  int n = std::min(max_buttons, kJoyButtons);
  std::memcpy(buttons, impl->read_buf.joy_buttons, static_cast<size_t>(n) * sizeof(bool));
  return n;
}

static bool VIsFallen(sdk_handle_t /*h*/) {
  return false;
}

static int VGetMode(sdk_handle_t /*h*/) {
  return SDK_ERR_NOT_SUPPORTED;
}

static int VGetCommStatus(sdk_handle_t h) {
  auto* impl = Cast(h);
  if (!impl) {
    return SDK_ERR_INVALID_ARG;
  }
  uint64_t count = impl->state_cb_count.load(std::memory_order_relaxed);
  if (count == 0) {
    return 1;
  }
  int64_t last = impl->last_state_cb_us.load(std::memory_order_relaxed);
  int64_t age_us = NowUs() - last;
  if (age_us > 2000000) {
    return 2;
  }
  if (age_us > 500000) {
    return 3;
  }
  return SDK_OK;
}

static float VGetMotorTemp(sdk_handle_t /*h*/, int /*idx*/) {
  return 0.0f;
}

static int VGetMotorMode(sdk_handle_t /*h*/, int /*idx*/) {
  return 0;
}

static int QueryMotionServiceName(SdkHandleImpl* impl, std::string* out_name) {
  std::string form;
  std::string name;
  int32_t rc = impl->motion_switcher->CheckMode(form, name);
  if (rc != 0) {
    std::fprintf(stderr, "unitree_g1_wrapper: CheckMode failed, raw=%d\n",
                 static_cast<int>(rc));
    return SDK_ERR_INTERNAL;
  }
  if (out_name) {
    *out_name = name;
  }
  return SDK_OK;
}

static int RequestReady(SdkHandleImpl* impl) {
  std::string active_name;
  if (QueryMotionServiceName(impl, &active_name) != SDK_OK) {
    return SDK_ERR_INTERNAL;
  }
  if (active_name.empty()) {
    return SDK_OK;
  }

  std::fprintf(stderr, "unitree_g1_wrapper: active motion service '%s', releasing\n",
               active_name.c_str());
  for (int retry = 0; retry < impl->max_release_retries; retry++) {
    int32_t rc = impl->motion_switcher->ReleaseMode();
    if (rc == 0) {
      break;
    }
    std::fprintf(stderr, "unitree_g1_wrapper: ReleaseMode failed, raw=%d retry=%d/%d\n",
                 static_cast<int>(rc), retry + 1, impl->max_release_retries);
    std::this_thread::sleep_for(std::chrono::seconds(impl->retry_interval_s));
  }

  active_name.clear();
  if (QueryMotionServiceName(impl, &active_name) != SDK_OK) {
    return SDK_ERR_INTERNAL;
  }
  if (!active_name.empty()) {
    std::fprintf(stderr, "unitree_g1_wrapper: motion service still active: %s\n",
                 active_name.c_str());
    return SDK_ERR_INTERNAL;
  }
  return SDK_OK;
}

static int RequestUnready(SdkHandleImpl* impl) {
  if (!impl->loco_client) {
    return SDK_ERR_INTERNAL;
  }

  std::string active_name;
  if (QueryMotionServiceName(impl, &active_name) != SDK_OK) {
    return SDK_ERR_INTERNAL;
  }
  if (!active_name.empty()) {
    int32_t damp_rc = impl->loco_client->Damp();
    if (damp_rc != 0) {
      std::fprintf(stderr, "unitree_g1_wrapper: Damp failed, raw=%d\n",
                   static_cast<int>(damp_rc));
      return SDK_ERR_INTERNAL;
    }
    return SDK_OK;
  }

  int32_t select_rc = impl->motion_switcher->SelectMode(impl->unready_mode_alias);
  if (select_rc != 0) {
    std::fprintf(stderr, "unitree_g1_wrapper: SelectMode(%s) failed, raw=%d\n",
                 impl->unready_mode_alias.c_str(), static_cast<int>(select_rc));
    return SDK_ERR_INTERNAL;
  }

  for (int retry = 0; retry < impl->max_release_retries; ++retry) {
    active_name.clear();
    if (QueryMotionServiceName(impl, &active_name) != SDK_OK) {
      return SDK_ERR_INTERNAL;
    }
    if (!active_name.empty()) {
      int32_t damp_rc = impl->loco_client->Damp();
      if (damp_rc != 0) {
        std::fprintf(stderr, "unitree_g1_wrapper: Damp failed, raw=%d\n",
                     static_cast<int>(damp_rc));
        return SDK_ERR_INTERNAL;
      }
      return SDK_OK;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::fprintf(stderr, "unitree_g1_wrapper: motion service did not become active after SelectMode(%s)\n",
               impl->unready_mode_alias.c_str());
  return SDK_ERR_INTERNAL;
}

static int VRequestControl(sdk_handle_t h, sdk_control_state_t target) {
  if (target != SDK_CONTROL_READY && target != SDK_CONTROL_UNREADY) {
    return SDK_ERR_INVALID_ARG;
  }
  auto* impl = Cast(h);
  if (!impl || !impl->motion_switcher) {
    return SDK_ERR_INTERNAL;
  }
  if (target == SDK_CONTROL_UNREADY) {
    return RequestUnready(impl);
  }

  return RequestReady(impl);
}

static sdk_control_state_t VGetControlState(sdk_handle_t h) {
  auto* impl = Cast(h);
  if (!impl || !impl->motion_switcher) {
    return SDK_CONTROL_UNKNOWN;
  }

  std::string form;
  std::string name;
  int32_t rc = impl->motion_switcher->CheckMode(form, name);
  if (rc != 0) {
    return SDK_CONTROL_UNKNOWN;
  }
  return name.empty() ? SDK_CONTROL_READY : SDK_CONTROL_UNREADY;
}

static int VSetStateCallback(sdk_handle_t /*h*/, sdk_vtable_t::sdk_state_callback_t /*cb*/,
                             void* /*user*/) {
  return SDK_ERR_NOT_SUPPORTED;
}

static int VSetJoyCallback(sdk_handle_t /*h*/, sdk_vtable_t::sdk_joy_callback_t /*cb*/,
                           void* /*user*/) {
  return SDK_ERR_NOT_SUPPORTED;
}

static void* VGetProcAddr(sdk_handle_t /*h*/, const char* /*symbol_name*/) {
  return nullptr;
}

static bool VHasNativeQuat(sdk_handle_t /*h*/) {
  return true;
}

// ============================================================================
// Exported vtable
// ============================================================================

SDK_API extern const sdk_vtable_t sdk_vtable = {
    /* struct_size     = */ sizeof(sdk_vtable_t),
    /* api_version     = */ SDK_API_VERSION,

    /* create          = */ VCreate,
    /* destroy         = */ VDestroy,

    /* get_num_motors  = */ VGetNumMotors,
    /* get_capabilities= */ VGetCapabilities,
    /* get_version     = */ VGetVersion,

    /* update          = */ VUpdate,

    /* get_motor_q     = */ VGetMotorQ,
    /* get_motor_dq    = */ VGetMotorDq,
    /* get_motor_tau   = */ VGetMotorTau,

    /* get_motor_q_batch    = */ VGetMotorQBatch,
    /* get_motor_dq_batch   = */ VGetMotorDqBatch,
    /* get_motor_tau_batch  = */ VGetMotorTauBatch,

    /* begin_cmd       = */ VBeginCmd,
    /* set_motor_cmd   = */ VSetMotorCmd,
    /* commit_cmd      = */ VCommitCmd,

    /* has_imu         = */ VHasImu,
    /* has_joystick    = */ VHasJoystick,
    /* has_battery     = */ VHasBattery,
    /* has_foot_sensor = */ VHasFootSensor,
    /* has_odometry    = */ VHasOdometry,
    /* has_hand        = */ VHasHand,

    /* get_imu_quat    = */ VGetImuQuat,
    /* get_imu_gyro    = */ VGetImuGyro,
    /* get_imu_rpy     = */ VGetImuRpy,
    /* get_imu_acc     = */ VGetImuAcc,

    /* get_battery_voltage   = */ VGetBatteryVoltage,
    /* get_battery_current   = */ VGetBatteryCurrent,
    /* get_battery_percentage= */ VGetBatteryPercentage,

    /* get_foot_force   = */ VGetFootForce,
    /* get_foot_contact = */ VGetFootContact,

    /* get_odom_pos     = */ VGetOdomPos,
    /* get_odom_vel     = */ VGetOdomVel,

    /* get_hand_joint_pos= */ VGetHandJointPos,

    /* get_joy_axes    = */ VGetJoyAxes,
    /* get_joy_buttons = */ VGetJoyButtons,

    /* is_fallen       = */ VIsFallen,
    /* get_mode        = */ VGetMode,
    /* get_comm_status = */ VGetCommStatus,

    /* get_motor_temp  = */ VGetMotorTemp,
    /* get_motor_mode  = */ VGetMotorMode,
    /* request_control   = */ VRequestControl,
    /* get_control_state = */ VGetControlState,
    /* set_state_callback = */ VSetStateCallback,
    /* set_joy_callback   = */ VSetJoyCallback,

    /* get_proc_addr   = */ VGetProcAddr,

    /* has_native_quat = */ VHasNativeQuat,
};
