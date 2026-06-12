/**
 * @file unitree_g1_rmw_wrapper.cpp
 * @brief Unitree G1 SDK wrapper implemented directly on ROS 2 RMW.
 *
 * This avoids rclcpp/rcl executors while still using ROS 2 topic mapping,
 * typesupport, QoS translation, and the selected RMW implementation.
 */

#include <unitree_api/msg/request.h>
#include <unitree_api/msg/response.h>
#include <unitree_hg/msg/low_cmd.h>
#include <unitree_api/msg/detail/request__rosidl_typesupport_fastrtps_c.h>
#include <unitree_api/msg/detail/request__rosidl_typesupport_introspection_c.h>
#include <unitree_api/msg/detail/response__rosidl_typesupport_fastrtps_c.h>
#include <unitree_api/msg/detail/response__rosidl_typesupport_introspection_c.h>
#include <unitree_hg/msg/detail/low_cmd__rosidl_typesupport_fastrtps_c.h>
#include <unitree_hg/msg/detail/low_cmd__rosidl_typesupport_introspection_c.h>
#include <unitree_hg/msg/low_state.h>
#include <unitree_hg/msg/detail/low_state__rosidl_typesupport_fastrtps_c.h>
#include <unitree_hg/msg/detail/low_state__rosidl_typesupport_introspection_c.h>

#include <rcl/error_handling.h>
#include <rcl/rcl.h>
#include <rcutils/allocator.h>
#include <rmw/error_handling.h>
#include <rmw/publisher_options.h>
#include <rmw/qos_profiles.h>
#include <rmw/subscription_options.h>
#include <rosidl_runtime_c/string_functions.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <mutex>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "sdk_wrapper/interface/sdk_wrapper_interface.h"

namespace {

constexpr int kG1NumMotors = 29;
constexpr int kJoyAxes = 4;
constexpr int kJoyButtons = 12;
constexpr uint32_t kCapabilities =
    SDK_CAP_LOW_LEVEL | SDK_CAP_IMU | SDK_CAP_JOYSTICK | SDK_CAP_NATIVE_QUAT;
constexpr uint8_t kModePr = 0;
constexpr uint8_t kMotorEnable = 1;

#pragma pack(push, 1)
union G1KeySwitch {
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
};

struct G1RemoteData {
  uint8_t head[2];
  G1KeySwitch btn;
  float lx;
  float rx;
  float ry;
  float L2;
  float ly;
  uint8_t idle[16];
};
#pragma pack(pop)

struct SdkDebugStats {
  uint64_t state_cb_count;
  uint64_t joy_cb_count;
  uint64_t update_count;
  uint64_t cmd_count;
  int64_t last_state_cb_us;
  int64_t last_joy_cb_us;
  int64_t now_us;
};

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
  uint8_t mode_pr{kModePr};
};

struct RawMotorCmd {
  uint8_t mode;
  float q;
  float dq;
  float tau;
  float kp;
  float kd;
  uint32_t reserve;
};

struct RawLowCmd {
  uint8_t mode_pr;
  uint8_t mode_machine;
  std::array<RawMotorCmd, 35> motor_cmd;
  std::array<uint32_t, 4> reserve;
  uint32_t crc;
};

struct SdkHandleImpl {
  rcl_context_t context = rcl_get_zero_initialized_context();
  rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
  rcl_node_t node = rcl_get_zero_initialized_node();
  rcl_subscription_t state_sub = rcl_get_zero_initialized_subscription();
  rcl_publisher_t cmd_pub = rcl_get_zero_initialized_publisher();
  rcl_publisher_t api_request_pub = rcl_get_zero_initialized_publisher();
  rcl_subscription_t api_response_sub = rcl_get_zero_initialized_subscription();
  rcl_publisher_t sport_request_pub = rcl_get_zero_initialized_publisher();
  rcl_subscription_t sport_response_sub = rcl_get_zero_initialized_subscription();
  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  rcl_wait_set_t api_wait_set = rcl_get_zero_initialized_wait_set();
  rcl_wait_set_t sport_wait_set = rcl_get_zero_initialized_wait_set();
  bool rcl_inited{false};
  bool node_inited{false};
  bool state_sub_inited{false};
  bool cmd_pub_inited{false};
  bool api_request_pub_inited{false};
  bool api_response_sub_inited{false};
  bool sport_request_pub_inited{false};
  bool sport_response_sub_inited{false};
  bool wait_set_inited{false};
  bool api_wait_set_inited{false};
  bool sport_wait_set_inited{false};

  std::thread state_thread;
  std::atomic<bool> running{false};

  std::mutex state_mutex;
  StateSnapshot write_buf;
  StateSnapshot read_buf;
  std::atomic<bool> new_data{false};

  std::mutex cmd_mutex;
  std::mutex api_mutex;
  std::mutex sport_mutex;
  float cmd_q[kG1NumMotors]{};
  float cmd_dq[kG1NumMotors]{};
  float cmd_tau[kG1NumMotors]{};
  float cmd_kp[kG1NumMotors]{};
  float cmd_kd[kG1NumMotors]{};

  std::atomic<uint64_t> state_cb_count{0};
  std::atomic<uint64_t> joy_cb_count{0};
  std::atomic<uint64_t> crc_fail_count{0};
  std::atomic<uint64_t> update_count{0};
  std::atomic<uint64_t> cmd_count{0};
  std::atomic<int64_t> last_state_cb_us{0};
  std::atomic<int64_t> last_joy_cb_us{0};
  std::atomic<uint8_t> latest_mode_machine{0};
  std::atomic<uint8_t> latest_mode_pr{kModePr};
  int64_t api_check_mode{1001};
  int64_t api_select_mode{1002};
  int64_t api_release_mode{1003};
  int64_t api_timeout_s{5};
  int64_t max_release_retries{5};
  int64_t retry_interval_s{3};
  int64_t loco_set_fsm_id{7101};
  int64_t loco_damp_fsm_id{1};
  std::string unready_mode_alias{"ai"};
};

}  // namespace

struct sdk_handle_s : SdkHandleImpl {};

namespace {

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

SdkHandleImpl* Cast(sdk_handle_t h) {
  return static_cast<SdkHandleImpl*>(h);
}

bool ValidIndex(int idx) {
  return idx >= 0 && idx < kG1NumMotors;
}

void LogRclError(const char* where) {
  const char* msg = rcl_get_error_string().str;
  std::fprintf(stderr, "unitree_g1_rmw_wrapper: %s failed: %s\n", where,
               msg ? msg : "<no rcl error>");
  rcl_reset_error();
}

void ForceLoadUnitreeTypeSupportLibraries() {
  const rosidl_message_type_support_t* handles[] = {
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
          rosidl_typesupport_introspection_c, unitree_api, msg, Request)(),
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
          rosidl_typesupport_introspection_c, unitree_api, msg, Response)(),
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_fastrtps_c,
                                                       unitree_api, msg, Request)(),
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_fastrtps_c,
                                                       unitree_api, msg, Response)(),
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
          rosidl_typesupport_introspection_c, unitree_hg, msg, LowState)(),
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
          rosidl_typesupport_introspection_c, unitree_hg, msg, LowCmd)(),
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_fastrtps_c,
                                                       unitree_hg, msg, LowState)(),
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(rosidl_typesupport_fastrtps_c,
                                                       unitree_hg, msg, LowCmd)(),
  };
  for (const auto* handle : handles) {
    (void)handle;
  }
}

std::string ExtractJsonStringField(const char* json, const char* field) {
  if (!json || !field) {
    return {};
  }
  std::string key = "\"";
  key += field;
  key += "\"";
  const char* pos = std::strstr(json, key.c_str());
  if (!pos) {
    return {};
  }
  pos = std::strchr(pos + key.size(), ':');
  if (!pos) {
    return {};
  }
  ++pos;
  while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') {
    ++pos;
  }
  if (*pos != '"') {
    return {};
  }
  ++pos;

  std::string out;
  bool escaping = false;
  for (; *pos; ++pos) {
    if (escaping) {
      out.push_back(*pos);
      escaping = false;
      continue;
    }
    if (*pos == '\\') {
      escaping = true;
      continue;
    }
    if (*pos == '"') {
      break;
    }
    out.push_back(*pos);
  }
  return out;
}

void DrainApiResponses(rcl_subscription_t* sub, bool sub_inited) {
  if (!sub || !sub_inited) {
    return;
  }

  unitree_api__msg__Response stale;
  unitree_api__msg__Response__init(&stale);
  while (true) {
    const rcl_ret_t rc = rcl_take(sub, &stale, nullptr, nullptr);
    if (rc == RCL_RET_OK) {
      continue;
    }
    if (rc == RCL_RET_SUBSCRIPTION_TAKE_FAILED) {
      rcl_reset_error();
    } else {
      LogRclError("rcl_take(stale api response)");
    }
    break;
  }
  unitree_api__msg__Response__fini(&stale);
}

int64_t MakeRequestIdentity() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Generic Unitree api request/response round-trip over a request publisher and a
// response subscription. Used for both the motion_switcher and sport services.
int CallApiService(SdkHandleImpl* h, rcl_publisher_t* req_pub, bool req_pub_inited,
                   rcl_subscription_t* resp_sub, bool resp_sub_inited,
                   rcl_wait_set_t* wait_set, bool wait_set_inited, std::mutex* mtx,
                   int64_t api_id, const char* parameter, std::string* data_out) {
  if (!h || !req_pub || !resp_sub || !wait_set || !mtx || !req_pub_inited ||
      !resp_sub_inited || !wait_set_inited) {
    return SDK_ERR_INTERNAL;
  }

  std::lock_guard<std::mutex> lock(*mtx);
  DrainApiResponses(resp_sub, resp_sub_inited);

  const int64_t identity = MakeRequestIdentity();
  unitree_api__msg__Request req;
  unitree_api__msg__Request__init(&req);
  req.header.identity.id = identity;
  req.header.identity.api_id = api_id;
  req.header.policy.noreply = false;
  req.header.policy.priority = 0;
  if (parameter && parameter[0] != '\0') {
    rosidl_runtime_c__String__assign(&req.parameter, parameter);
  }

  const rcl_ret_t publish_rc = rcl_publish(req_pub, &req, nullptr);
  unitree_api__msg__Request__fini(&req);
  if (publish_rc != RCL_RET_OK) {
    LogRclError("rcl_publish(api request)");
    return SDK_ERR_COMM;
  }

  unitree_api__msg__Response resp;
  unitree_api__msg__Response__init(&resp);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(h->api_timeout_s);
  int result = SDK_ERR_TIMEOUT;

  while (std::chrono::steady_clock::now() < deadline) {
    if (rcl_wait_set_clear(wait_set) != RCL_RET_OK) {
      LogRclError("rcl_wait_set_clear(api)");
      result = SDK_ERR_INTERNAL;
      break;
    }
    if (rcl_wait_set_add_subscription(wait_set, resp_sub, nullptr) != RCL_RET_OK) {
      LogRclError("rcl_wait_set_add_subscription(api)");
      result = SDK_ERR_INTERNAL;
      break;
    }

    const rcl_ret_t wait_rc = rcl_wait(wait_set, 200000000);
    if (wait_rc == RCL_RET_TIMEOUT) {
      continue;
    }
    if (wait_rc != RCL_RET_OK) {
      LogRclError("rcl_wait(api)");
      result = SDK_ERR_COMM;
      break;
    }
    if (wait_set->subscriptions[0] == nullptr) {
      continue;
    }

    while (true) {
      const rcl_ret_t take_rc = rcl_take(resp_sub, &resp, nullptr, nullptr);
      if (take_rc == RCL_RET_SUBSCRIPTION_TAKE_FAILED) {
        rcl_reset_error();
        break;
      }
      if (take_rc != RCL_RET_OK) {
        LogRclError("rcl_take(api response)");
        result = SDK_ERR_COMM;
        break;
      }
      if (resp.header.identity.id != identity) {
        continue;
      }
      if (resp.header.status.code != 0) {
        std::fprintf(stderr,
                     "unitree_g1_rmw_wrapper: api %" PRId64 " returned status %d\n",
                     api_id, resp.header.status.code);
        result = SDK_ERR_COMM;
      } else {
        if (data_out) {
          *data_out = resp.data.data ? resp.data.data : "";
        }
        result = SDK_OK;
      }
      goto done;
    }
    if (result == SDK_ERR_COMM || result == SDK_ERR_INTERNAL) {
      break;
    }
  }

done:
  unitree_api__msg__Response__fini(&resp);
  return result;
}

int CallMotionSwitcher(SdkHandleImpl* h, int64_t api_id, const char* parameter,
                       std::string* data_out) {
  return CallApiService(h, &h->api_request_pub, h->api_request_pub_inited,
                        &h->api_response_sub, h->api_response_sub_inited, &h->api_wait_set,
                        h->api_wait_set_inited, &h->api_mutex, api_id, parameter, data_out);
}

int CallSport(SdkHandleImpl* h, int64_t api_id, const char* parameter, std::string* data_out) {
  return CallApiService(h, &h->sport_request_pub, h->sport_request_pub_inited,
                        &h->sport_response_sub, h->sport_response_sub_inited,
                        &h->sport_wait_set, h->sport_wait_set_inited, &h->sport_mutex, api_id,
                        parameter, data_out);
}

int CheckMotionMode(SdkHandleImpl* h, std::string* active_name, std::string* active_form) {
  std::string data;
  const int rc = CallMotionSwitcher(h, h->api_check_mode, "", &data);
  if (rc != SDK_OK) {
    return rc;
  }
  if (active_name) {
    *active_name = ExtractJsonStringField(data.c_str(), "name");
  }
  if (active_form) {
    *active_form = ExtractJsonStringField(data.c_str(), "form");
  }
  return SDK_OK;
}

int LocoDamp(SdkHandleImpl* h) {
  const std::string param = "{\"data\":" + std::to_string(h->loco_damp_fsm_id) + "}";
  const int rc = CallSport(h, h->loco_set_fsm_id, param.c_str(), nullptr);
  if (rc != SDK_OK) {
    std::fprintf(stderr, "unitree_g1_rmw_wrapper: Damp failed (%d)\n", rc);
  }
  return rc;
}

// READY: release whatever motion service currently owns the robot so low-level
// control can publish (mirrors the SDK MotionSwitcherClient release flow).
int RequestReady(SdkHandleImpl* h) {
  std::string form;
  std::string name;
  if (CheckMotionMode(h, &name, &form) != SDK_OK) {
    return SDK_ERR_INTERNAL;
  }
  if (name.empty()) {
    return SDK_OK;
  }

  std::fprintf(stderr, "unitree_g1_rmw_wrapper: active motion service '%s', releasing\n",
               name.c_str());
  for (int retry = 0; retry < h->max_release_retries; ++retry) {
    if (CallMotionSwitcher(h, h->api_release_mode, "", nullptr) == SDK_OK) {
      break;
    }
    std::fprintf(stderr, "unitree_g1_rmw_wrapper: ReleaseMode failed, retry=%d/%" PRId64 "\n",
                 retry + 1, h->max_release_retries);
    std::this_thread::sleep_for(std::chrono::seconds(h->retry_interval_s));
  }

  name.clear();
  if (CheckMotionMode(h, &name, &form) != SDK_OK) {
    return SDK_ERR_INTERNAL;
  }
  if (!name.empty()) {
    std::fprintf(stderr, "unitree_g1_rmw_wrapper: motion service still active: %s\n",
                 name.c_str());
    return SDK_ERR_INTERNAL;
  }
  return SDK_OK;
}

// UNREADY: hand the robot back to a safe damping state (mirrors the SDK
// LocoClient::Damp / MotionSwitcherClient::SelectMode flow). If a motion
// service is already active, Damp it directly; otherwise select the configured
// fallback mode, wait for it to come up, then Damp.
int RequestUnready(SdkHandleImpl* h) {
  std::string form;
  std::string name;
  if (CheckMotionMode(h, &name, &form) != SDK_OK) {
    return SDK_ERR_INTERNAL;
  }
  if (!name.empty()) {
    return LocoDamp(h);
  }

  const std::string select_param = "{\"name\":\"" + h->unready_mode_alias + "\"}";
  if (CallMotionSwitcher(h, h->api_select_mode, select_param.c_str(), nullptr) != SDK_OK) {
    std::fprintf(stderr, "unitree_g1_rmw_wrapper: SelectMode(%s) failed\n",
                 h->unready_mode_alias.c_str());
    return SDK_ERR_INTERNAL;
  }

  for (int retry = 0; retry < h->max_release_retries; ++retry) {
    name.clear();
    if (CheckMotionMode(h, &name, &form) != SDK_OK) {
      return SDK_ERR_INTERNAL;
    }
    if (!name.empty()) {
      return LocoDamp(h);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::fprintf(stderr,
               "unitree_g1_rmw_wrapper: motion service did not become active after SelectMode(%s)\n",
               h->unready_mode_alias.c_str());
  return SDK_ERR_INTERNAL;
}

uint32_t Crc32Core(const uint32_t* ptr, uint32_t len) {
  uint32_t xbit = 0;
  uint32_t data = 0;
  uint32_t crc = 0xFFFFFFFF;
  constexpr uint32_t kPolynomial = 0x04c11db7;
  for (uint32_t i = 0; i < len; i++) {
    xbit = 1U << 31;
    data = ptr[i];
    for (uint32_t bits = 0; bits < 32; bits++) {
      if ((crc & 0x80000000U) != 0U) {
        crc <<= 1;
        crc ^= kPolynomial;
      } else {
        crc <<= 1;
      }
      if ((data & xbit) != 0U) {
        crc ^= kPolynomial;
      }
      xbit >>= 1;
    }
  }
  return crc;
}

bool VerifyLowStateCrc(const unitree_hg__msg__LowState& state) {
  unitree_hg__msg__LowState copy = state;
  const uint32_t expected = copy.crc;
  const uint32_t computed =
      Crc32Core(reinterpret_cast<const uint32_t*>(&copy), (sizeof(copy) >> 2) - 1);
  return expected == computed;
}

void ApplyLowCmdCrc(unitree_hg__msg__LowCmd* cmd) {
  if (!cmd) {
    return;
  }

  RawLowCmd raw;
  std::memset(&raw, 0, sizeof(raw));
  raw.mode_pr = cmd->mode_pr;
  raw.mode_machine = cmd->mode_machine;
  for (int i = 0; i < 35; ++i) {
    raw.motor_cmd[i].mode = cmd->motor_cmd[i].mode;
    raw.motor_cmd[i].q = cmd->motor_cmd[i].q;
    raw.motor_cmd[i].dq = cmd->motor_cmd[i].dq;
    raw.motor_cmd[i].tau = cmd->motor_cmd[i].tau;
    raw.motor_cmd[i].kp = cmd->motor_cmd[i].kp;
    raw.motor_cmd[i].kd = cmd->motor_cmd[i].kd;
    raw.motor_cmd[i].reserve = cmd->motor_cmd[i].reserve;
  }
  std::memcpy(&raw.reserve[0], &cmd->reserve[0], sizeof(uint32_t));
  cmd->crc = Crc32Core(reinterpret_cast<const uint32_t*>(&raw), (sizeof(raw) >> 2) - 1);
}

void ParseJoystick(const uint8_t remote[40], StateSnapshot* out) {
  if (!remote || !out) {
    return;
  }

  G1RemoteData raw{};
  std::memcpy(&raw, remote, sizeof(raw));

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

void HandleLowState(SdkHandleImpl* h, const unitree_hg__msg__LowState& state) {
  if (!h) {
    return;
  }

  std::lock_guard<std::mutex> lock(h->state_mutex);
  for (int i = 0; i < kG1NumMotors; ++i) {
    h->write_buf.motor_q[i] = state.motor_state[i].q;
    h->write_buf.motor_dq[i] = state.motor_state[i].dq;
    h->write_buf.motor_tau[i] = state.motor_state[i].tau_est;
  }

  for (int i = 0; i < 4; ++i) {
    h->write_buf.imu_quat[i] = state.imu_state.quaternion[i];
  }
  for (int i = 0; i < 3; ++i) {
    h->write_buf.imu_rpy[i] = state.imu_state.rpy[i];
    h->write_buf.imu_gyro[i] = state.imu_state.gyroscope[i];
    h->write_buf.imu_acc[i] = state.imu_state.accelerometer[i];
  }

  h->write_buf.mode_machine = state.mode_machine;
  h->write_buf.mode_pr = state.mode_pr;
  ParseJoystick(state.wireless_remote, &h->write_buf);

  h->latest_mode_machine.store(state.mode_machine, std::memory_order_relaxed);
  h->latest_mode_pr.store(state.mode_pr, std::memory_order_relaxed);
  h->new_data.store(true, std::memory_order_release);
  h->state_cb_count.fetch_add(1, std::memory_order_relaxed);
  h->joy_cb_count.fetch_add(1, std::memory_order_relaxed);
  const int64_t now = NowUs();
  h->last_state_cb_us.store(now, std::memory_order_relaxed);
  h->last_joy_cb_us.store(now, std::memory_order_relaxed);
}

void StateThread(SdkHandleImpl* h) {
  unitree_hg__msg__LowState msg;
  unitree_hg__msg__LowState__init(&msg);

  while (h->running.load(std::memory_order_acquire)) {
    if (rcl_wait_set_clear(&h->wait_set) != RCL_RET_OK) {
      LogRclError("rcl_wait_set_clear");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (rcl_wait_set_add_subscription(&h->wait_set, &h->state_sub, nullptr) != RCL_RET_OK) {
      LogRclError("rcl_wait_set_add_subscription");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    const rcl_ret_t wait_rc = rcl_wait(&h->wait_set, 100000000);
    if (!h->running.load(std::memory_order_acquire)) {
      break;
    }
    if (wait_rc == RCL_RET_TIMEOUT) {
      continue;
    }
    if (wait_rc != RCL_RET_OK) {
      LogRclError("rcl_wait");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (h->wait_set.subscriptions[0] == nullptr) {
      continue;
    }

    while (h->running.load(std::memory_order_acquire)) {
      const rcl_ret_t take_rc = rcl_take(&h->state_sub, &msg, nullptr, nullptr);
      if (take_rc == RCL_RET_SUBSCRIPTION_TAKE_FAILED) {
        rcl_reset_error();
        break;
      }
      if (take_rc != RCL_RET_OK) {
        LogRclError("rcl_take");
        break;
      }
      if (!VerifyLowStateCrc(msg)) {
        const uint64_t crc_fail = h->crc_fail_count.fetch_add(1, std::memory_order_relaxed);
        if (crc_fail < 3) {
          std::fprintf(stderr,
                       "unitree_g1_rmw_wrapper: LowState CRC mismatch; dropping frame (%lu)\n",
                       static_cast<unsigned long>(crc_fail + 1));
        }
        continue;
      }
      HandleLowState(h, msg);
    }
  }

  unitree_hg__msg__LowState__fini(&msg);
}

std::string MakeCycloneUri(const char* net_if) {
  if (!net_if || net_if[0] == '\0') {
    return {};
  }
  std::string uri = "<CycloneDDS><Domain><General><Interfaces>"
                    "<NetworkInterface name=\"";
  uri += net_if;
  uri += "\" priority=\"default\" multicast=\"default\" />"
         "</Interfaces></General></Domain></CycloneDDS>";
  return uri;
}

void CleanupRmw(SdkHandleImpl* h) {
  if (!h) {
    return;
  }

  h->running.store(false, std::memory_order_release);
  if (h->state_thread.joinable()) {
    h->state_thread.join();
  }

  if (h->wait_set_inited) {
    const rcl_ret_t rc = rcl_wait_set_fini(&h->wait_set);
    (void)rc;
    h->wait_set_inited = false;
  }
  if (h->api_wait_set_inited) {
    const rcl_ret_t rc = rcl_wait_set_fini(&h->api_wait_set);
    (void)rc;
    h->api_wait_set_inited = false;
  }
  if (h->sport_wait_set_inited) {
    const rcl_ret_t rc = rcl_wait_set_fini(&h->sport_wait_set);
    (void)rc;
    h->sport_wait_set_inited = false;
  }
  if (h->cmd_pub_inited) {
    const rcl_ret_t rc = rcl_publisher_fini(&h->cmd_pub, &h->node);
    (void)rc;
    h->cmd_pub_inited = false;
  }
  if (h->api_request_pub_inited) {
    const rcl_ret_t rc = rcl_publisher_fini(&h->api_request_pub, &h->node);
    (void)rc;
    h->api_request_pub_inited = false;
  }
  if (h->api_response_sub_inited) {
    const rcl_ret_t rc = rcl_subscription_fini(&h->api_response_sub, &h->node);
    (void)rc;
    h->api_response_sub_inited = false;
  }
  if (h->sport_request_pub_inited) {
    const rcl_ret_t rc = rcl_publisher_fini(&h->sport_request_pub, &h->node);
    (void)rc;
    h->sport_request_pub_inited = false;
  }
  if (h->sport_response_sub_inited) {
    const rcl_ret_t rc = rcl_subscription_fini(&h->sport_response_sub, &h->node);
    (void)rc;
    h->sport_response_sub_inited = false;
  }
  if (h->state_sub_inited) {
    const rcl_ret_t rc = rcl_subscription_fini(&h->state_sub, &h->node);
    (void)rc;
    h->state_sub_inited = false;
  }
  if (h->node_inited) {
    const rcl_ret_t rc = rcl_node_fini(&h->node);
    (void)rc;
    h->node_inited = false;
  }
  if (h->rcl_inited) {
    const rcl_ret_t shutdown_rc = rcl_shutdown(&h->context);
    const rcl_ret_t fini_rc = rcl_context_fini(&h->context);
    (void)shutdown_rc;
    (void)fini_rc;
    h->rcl_inited = false;
  }
  if (h->init_options.impl) {
    const rcl_ret_t rc = rcl_init_options_fini(&h->init_options);
    (void)rc;
  }
}

// =========== JSON helpers for config.json parsing ===========

static std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static std::string JsonStr(const std::string& json, const std::string& key,
                           const std::string& default_val = "") {
  const std::string search = "\"" + key + "\"";
  auto pos = json.find(search);
  if (pos == std::string::npos) return default_val;
  pos = json.find(':', pos + search.size());
  if (pos == std::string::npos) return default_val;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' ||
                               json[pos] == '\r')) {
    ++pos;
  }
  if (pos >= json.size()) return default_val;
  if (json[pos] == '"') {
    ++pos;
    std::string out;
    bool escape = false;
    while (pos < json.size()) {
      if (escape) {
        out += json[pos];
        escape = false;
        ++pos;
        continue;
      }
      if (json[pos] == '\\') {
        escape = true;
        ++pos;
        continue;
      }
      if (json[pos] == '"') break;
      out += json[pos];
      ++pos;
    }
    return out;
  }
  size_t end = pos;
  while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ' ' &&
         json[end] != '\t' && json[end] != '\n' && json[end] != '\r') {
    ++end;
  }
  return json.substr(pos, end - pos);
}

static int64_t JsonInt(const std::string& json, const std::string& key, int64_t default_val = 0) {
  const auto val = JsonStr(json, key);
  if (val.empty()) return default_val;
  return std::stoll(val);
}

int InitRmw(const std::string& config_json, const std::string& net_if, int domain_id, SdkHandleImpl* h) {
  if (!h) {
    return SDK_ERR_INVALID_ARG;
  }

  const char* local_only = std::getenv("ROS_LOCALHOST_ONLY");
  if (local_only && std::strcmp(local_only, "1") == 0) {
    std::fprintf(stderr,
                 "unitree_g1_rmw_wrapper: overriding ROS_LOCALHOST_ONLY=1 to 0 for hardware "
                 "DDS\n");
    setenv("ROS_LOCALHOST_ONLY", "0", 1);
  }

  // Read configurable values from config.json
  const std::string default_rmw = JsonStr(config_json, "default_rmw_implementation", "rmw_cyclonedds_cpp");
  const std::string node_name = JsonStr(config_json, "node_name", "robocraft_unitree_g1_rmw_wrapper");
  const std::string topic_low_state = JsonStr(config_json, "topic_low_state", "/lowstate");
  const std::string topic_low_cmd = JsonStr(config_json, "topic_low_cmd", "/lowcmd");
  const std::string topic_motion_switcher_request = JsonStr(config_json, "topic_motion_switcher_request", "/api/motion_switcher/request");
  const std::string topic_motion_switcher_response = JsonStr(config_json, "topic_motion_switcher_response", "/api/motion_switcher/response");
  const std::string topic_sport_request = JsonStr(config_json, "topic_sport_request", "/api/sport/request");
  const std::string topic_sport_response = JsonStr(config_json, "topic_sport_response", "/api/sport/response");
  h->api_check_mode = JsonInt(config_json, "api_check_mode", 1001);
  h->api_select_mode = JsonInt(config_json, "api_select_mode", 1002);
  h->api_release_mode = JsonInt(config_json, "api_release_mode", 1003);
  h->api_timeout_s = JsonInt(config_json, "api_timeout_s", 5);
  h->max_release_retries = JsonInt(config_json, "max_release_retries", 5);
  h->retry_interval_s = JsonInt(config_json, "retry_interval_s", 3);
  h->loco_set_fsm_id = JsonInt(config_json, "loco_set_fsm_id", 7101);
  h->loco_damp_fsm_id = JsonInt(config_json, "loco_damp_fsm_id", 1);
  h->unready_mode_alias = JsonStr(config_json, "unready_mode_alias", "ai");

  if (!std::getenv("RMW_IMPLEMENTATION")) {
    setenv("RMW_IMPLEMENTATION", default_rmw.c_str(), 0);
  }
  if (!std::getenv("CYCLONEDDS_URI")) {
    const std::string uri = MakeCycloneUri(net_if.c_str());
    if (!uri.empty()) {
      setenv("CYCLONEDDS_URI", uri.c_str(), 0);
    }
  }

  ForceLoadUnitreeTypeSupportLibraries();

  h->init_options = rcl_get_zero_initialized_init_options();
  rcl_allocator_t allocator = rcl_get_default_allocator();
  if (rcl_init_options_init(&h->init_options, allocator) != RCL_RET_OK) {
    LogRclError("rcl_init_options_init");
    return SDK_ERR_INTERNAL;
  }

  if (domain_id >= 0 &&
      rcl_init_options_set_domain_id(&h->init_options, static_cast<size_t>(domain_id)) !=
          RCL_RET_OK) {
    LogRclError("rcl_init_options_set_domain_id");
    return SDK_ERR_INTERNAL;
  }

  h->context = rcl_get_zero_initialized_context();
  if (rcl_init(0, nullptr, &h->init_options, &h->context) != RCL_RET_OK) {
    LogRclError("rcl_init");
    return SDK_ERR_INTERNAL;
  }
  h->rcl_inited = true;

  h->node = rcl_get_zero_initialized_node();
  rcl_node_options_t node_options = rcl_node_get_default_options();
  if (rcl_node_init(&h->node, node_name.c_str(), "/", &h->context,
                    &node_options) != RCL_RET_OK) {
    LogRclError("rcl_node_init");
    return SDK_ERR_INTERNAL;
  }
  h->node_inited = true;

  auto sub_qos = rmw_qos_profile_sensor_data;
  sub_qos.depth = 1;
  h->state_sub = rcl_get_zero_initialized_subscription();
  rcl_subscription_options_t sub_options = rcl_subscription_get_default_options();
  sub_options.qos = sub_qos;
  if (rcl_subscription_init(&h->state_sub, &h->node,
                            ROSIDL_GET_MSG_TYPE_SUPPORT(unitree_hg, msg, LowState),
                            topic_low_state.c_str(), &sub_options) != RCL_RET_OK) {
    LogRclError("rcl_subscription_init(lowstate)");
    return SDK_ERR_INTERNAL;
  }
  h->state_sub_inited = true;

  auto pub_qos = rmw_qos_profile_default;
  pub_qos.depth = 10;
  h->cmd_pub = rcl_get_zero_initialized_publisher();
  rcl_publisher_options_t pub_options = rcl_publisher_get_default_options();
  pub_options.qos = pub_qos;
  if (rcl_publisher_init(&h->cmd_pub, &h->node,
                         ROSIDL_GET_MSG_TYPE_SUPPORT(unitree_hg, msg, LowCmd), topic_low_cmd.c_str(),
                         &pub_options) != RCL_RET_OK) {
    LogRclError("rcl_publisher_init(lowcmd)");
    return SDK_ERR_INTERNAL;
  }
  h->cmd_pub_inited = true;

  auto api_qos = rmw_qos_profile_default;
  api_qos.depth = 1;
  h->api_request_pub = rcl_get_zero_initialized_publisher();
  rcl_publisher_options_t api_pub_options = rcl_publisher_get_default_options();
  api_pub_options.qos = api_qos;
  if (rcl_publisher_init(&h->api_request_pub, &h->node,
                         ROSIDL_GET_MSG_TYPE_SUPPORT(unitree_api, msg, Request),
                         topic_motion_switcher_request.c_str(), &api_pub_options) != RCL_RET_OK) {
    LogRclError("rcl_publisher_init(motion_switcher request)");
    return SDK_ERR_INTERNAL;
  }
  h->api_request_pub_inited = true;

  h->api_response_sub = rcl_get_zero_initialized_subscription();
  rcl_subscription_options_t api_sub_options = rcl_subscription_get_default_options();
  api_sub_options.qos = api_qos;
  if (rcl_subscription_init(&h->api_response_sub, &h->node,
                            ROSIDL_GET_MSG_TYPE_SUPPORT(unitree_api, msg, Response),
                            topic_motion_switcher_response.c_str(), &api_sub_options) != RCL_RET_OK) {
    LogRclError("rcl_subscription_init(motion_switcher response)");
    return SDK_ERR_INTERNAL;
  }
  h->api_response_sub_inited = true;

  h->sport_request_pub = rcl_get_zero_initialized_publisher();
  rcl_publisher_options_t sport_pub_options = rcl_publisher_get_default_options();
  sport_pub_options.qos = api_qos;
  if (rcl_publisher_init(&h->sport_request_pub, &h->node,
                         ROSIDL_GET_MSG_TYPE_SUPPORT(unitree_api, msg, Request),
                         topic_sport_request.c_str(), &sport_pub_options) != RCL_RET_OK) {
    LogRclError("rcl_publisher_init(sport request)");
    return SDK_ERR_INTERNAL;
  }
  h->sport_request_pub_inited = true;

  h->sport_response_sub = rcl_get_zero_initialized_subscription();
  rcl_subscription_options_t sport_sub_options = rcl_subscription_get_default_options();
  sport_sub_options.qos = api_qos;
  if (rcl_subscription_init(&h->sport_response_sub, &h->node,
                            ROSIDL_GET_MSG_TYPE_SUPPORT(unitree_api, msg, Response),
                            topic_sport_response.c_str(), &sport_sub_options) != RCL_RET_OK) {
    LogRclError("rcl_subscription_init(sport response)");
    return SDK_ERR_INTERNAL;
  }
  h->sport_response_sub_inited = true;

  h->wait_set = rcl_get_zero_initialized_wait_set();
  if (rcl_wait_set_init(&h->wait_set, 1, 0, 0, 0, 0, 0, &h->context, allocator) != RCL_RET_OK) {
    LogRclError("rcl_wait_set_init");
    return SDK_ERR_INTERNAL;
  }
  h->wait_set_inited = true;

  h->api_wait_set = rcl_get_zero_initialized_wait_set();
  if (rcl_wait_set_init(&h->api_wait_set, 1, 0, 0, 0, 0, 0, &h->context, allocator) !=
      RCL_RET_OK) {
    LogRclError("rcl_wait_set_init(motion_switcher)");
    return SDK_ERR_INTERNAL;
  }
  h->api_wait_set_inited = true;

  h->sport_wait_set = rcl_get_zero_initialized_wait_set();
  if (rcl_wait_set_init(&h->sport_wait_set, 1, 0, 0, 0, 0, 0, &h->context, allocator) !=
      RCL_RET_OK) {
    LogRclError("rcl_wait_set_init(sport)");
    return SDK_ERR_INTERNAL;
  }
  h->sport_wait_set_inited = true;

  h->running.store(true, std::memory_order_release);
  h->state_thread = std::thread(StateThread, h);
  return SDK_OK;
}

int VCreate(const char* wrapper_dir, sdk_handle_t* out) {
  if (!out) {
    return SDK_ERR_INVALID_ARG;
  }
  *out = nullptr;

  const std::string config_json = ReadFile(std::string(wrapper_dir) + "/config.json");
  const std::string net_if = JsonStr(config_json, "network_interface", "");
  const int domain_id = static_cast<int>(JsonInt(config_json, "domain_id", 0));

  auto* h = new SdkHandleImpl();
  const int rc = InitRmw(config_json, net_if, domain_id, h);
  if (rc != SDK_OK) {
    CleanupRmw(h);
    delete h;
    return rc;
  }

  *out = static_cast<sdk_handle_t>(h);
  return SDK_OK;
}

void VDestroy(sdk_handle_t handle) {
  auto* h = Cast(handle);
  if (!h) {
    return;
  }
  CleanupRmw(h);
  delete h;
}

int VGetNumMotors(sdk_handle_t) {
  return kG1NumMotors;
}

uint32_t VGetCapabilities(sdk_handle_t) {
  return kCapabilities;
}

const char* VGetVersion(sdk_handle_t) {
  return "unitree-g1-rmw-wrapper-0.1";
}

int VUpdate(sdk_handle_t handle) {
  auto* h = Cast(handle);
  if (!h) {
    return SDK_ERR_INVALID_ARG;
  }
  h->update_count.fetch_add(1, std::memory_order_relaxed);
  if (h->new_data.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(h->state_mutex);
    h->read_buf = h->write_buf;
    h->new_data.store(false, std::memory_order_release);
  }
  return SDK_OK;
}

float VGetMotorQ(sdk_handle_t handle, int idx) {
  auto* h = Cast(handle);
  return (h && ValidIndex(idx)) ? h->read_buf.motor_q[idx] : 0.0f;
}

float VGetMotorDq(sdk_handle_t handle, int idx) {
  auto* h = Cast(handle);
  return (h && ValidIndex(idx)) ? h->read_buf.motor_dq[idx] : 0.0f;
}

float VGetMotorTau(sdk_handle_t handle, int idx) {
  auto* h = Cast(handle);
  return (h && ValidIndex(idx)) ? h->read_buf.motor_tau[idx] : 0.0f;
}

template <typename Field>
int CopyMotorBatch(sdk_handle_t handle, float* out, int max, Field field) {
  auto* h = Cast(handle);
  if (!h || !out) {
    return SDK_ERR_INVALID_ARG;
  }
  if (max <= 0) {
    return 0;
  }
  const int n = std::min(max, kG1NumMotors);
  for (int i = 0; i < n; ++i) {
    out[i] = field(h->read_buf, i);
  }
  return n;
}

int VGetMotorQBatch(sdk_handle_t h, float* out, int max) {
  return CopyMotorBatch(h, out, max, [](const StateSnapshot& s, int i) { return s.motor_q[i]; });
}

int VGetMotorDqBatch(sdk_handle_t h, float* out, int max) {
  return CopyMotorBatch(h, out, max, [](const StateSnapshot& s, int i) { return s.motor_dq[i]; });
}

int VGetMotorTauBatch(sdk_handle_t h, float* out, int max) {
  return CopyMotorBatch(h, out, max, [](const StateSnapshot& s, int i) { return s.motor_tau[i]; });
}

void VBeginCmd(sdk_handle_t handle) {
  auto* h = Cast(handle);
  if (!h) {
    return;
  }
  std::lock_guard<std::mutex> lock(h->cmd_mutex);
  std::memset(h->cmd_q, 0, sizeof(h->cmd_q));
  std::memset(h->cmd_dq, 0, sizeof(h->cmd_dq));
  std::memset(h->cmd_tau, 0, sizeof(h->cmd_tau));
  std::memset(h->cmd_kp, 0, sizeof(h->cmd_kp));
  std::memset(h->cmd_kd, 0, sizeof(h->cmd_kd));
}

void VSetMotorCmd(sdk_handle_t handle, int idx, float q, float dq, float tau, float kp, float kd) {
  auto* h = Cast(handle);
  if (!h || !ValidIndex(idx)) {
    return;
  }
  std::lock_guard<std::mutex> lock(h->cmd_mutex);
  h->cmd_q[idx] = q;
  h->cmd_dq[idx] = dq;
  h->cmd_tau[idx] = tau;
  h->cmd_kp[idx] = kp;
  h->cmd_kd[idx] = kd;
}

void VCommitCmd(sdk_handle_t handle) {
  auto* h = Cast(handle);
  if (!h || !h->cmd_pub_inited) {
    return;
  }

  unitree_hg__msg__LowCmd cmd;
  std::memset(&cmd, 0, sizeof(cmd));
  unitree_hg__msg__LowCmd__init(&cmd);
  cmd.mode_pr = h->latest_mode_pr.load(std::memory_order_relaxed);
  cmd.mode_machine = h->latest_mode_machine.load(std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(h->cmd_mutex);
    for (int i = 0; i < kG1NumMotors; ++i) {
      auto& m = cmd.motor_cmd[i];
      m.mode = kMotorEnable;
      m.q = h->cmd_q[i];
      m.dq = h->cmd_dq[i];
      m.tau = h->cmd_tau[i];
      m.kp = h->cmd_kp[i];
      m.kd = h->cmd_kd[i];
    }
  }

  ApplyLowCmdCrc(&cmd);
  if (rcl_publish(&h->cmd_pub, &cmd, nullptr) == RCL_RET_OK) {
    h->cmd_count.fetch_add(1, std::memory_order_relaxed);
  } else {
    LogRclError("rcl_publish(lowcmd)");
  }
  unitree_hg__msg__LowCmd__fini(&cmd);
}

bool VHasImu(sdk_handle_t) { return true; }
bool VHasJoystick(sdk_handle_t) { return true; }
bool VFalse(sdk_handle_t) { return false; }
bool VHasHand(sdk_handle_t, int) { return false; }

int VGetImuQuat(sdk_handle_t handle, float quat[4]) {
  auto* h = Cast(handle);
  if (!h || !quat) {
    return SDK_ERR_INVALID_ARG;
  }
  std::memcpy(quat, h->read_buf.imu_quat, 4 * sizeof(float));
  return SDK_OK;
}

int VGetImuGyro(sdk_handle_t handle, float gyro[3]) {
  auto* h = Cast(handle);
  if (!h || !gyro) {
    return SDK_ERR_INVALID_ARG;
  }
  std::memcpy(gyro, h->read_buf.imu_gyro, 3 * sizeof(float));
  return SDK_OK;
}

int VGetImuRpy(sdk_handle_t handle, float rpy[3]) {
  auto* h = Cast(handle);
  if (!h || !rpy) {
    return SDK_ERR_INVALID_ARG;
  }
  std::memcpy(rpy, h->read_buf.imu_rpy, 3 * sizeof(float));
  return SDK_OK;
}

int VGetImuAcc(sdk_handle_t handle, float acc[3]) {
  auto* h = Cast(handle);
  if (!h || !acc) {
    return SDK_ERR_INVALID_ARG;
  }
  std::memcpy(acc, h->read_buf.imu_acc, 3 * sizeof(float));
  return SDK_OK;
}

float VZeroFloat(sdk_handle_t) { return 0.0f; }
int VNotSupportedFootForce(sdk_handle_t, int, float*, int) { return SDK_ERR_NOT_SUPPORTED; }
int VNotSupportedFootContact(sdk_handle_t, int, bool*, int) { return SDK_ERR_NOT_SUPPORTED; }
int VNotSupportedOdom(sdk_handle_t, double[3]) { return SDK_ERR_NOT_SUPPORTED; }
int VNotSupportedHandPos(sdk_handle_t, int, float*, int) { return SDK_ERR_NOT_SUPPORTED; }

int VGetJoyAxes(sdk_handle_t handle, float* axes, int max_axes) {
  auto* h = Cast(handle);
  if (!h || !axes) {
    return SDK_ERR_INVALID_ARG;
  }
  if (max_axes <= 0) {
    return 0;
  }
  const int n = std::min(max_axes, kJoyAxes);
  std::memcpy(axes, h->read_buf.joy_axes, static_cast<size_t>(n) * sizeof(float));
  return n;
}

int VGetJoyButtons(sdk_handle_t handle, bool* buttons, int max_buttons) {
  auto* h = Cast(handle);
  if (!h || !buttons) {
    return SDK_ERR_INVALID_ARG;
  }
  if (max_buttons <= 0) {
    return 0;
  }
  const int n = std::min(max_buttons, kJoyButtons);
  std::memcpy(buttons, h->read_buf.joy_buttons, static_cast<size_t>(n) * sizeof(bool));
  return n;
}

bool VIsFallen(sdk_handle_t) { return false; }
int VGetMode(sdk_handle_t) { return SDK_ERR_NOT_SUPPORTED; }

int VGetCommStatus(sdk_handle_t handle) {
  auto* h = Cast(handle);
  if (!h) {
    return SDK_ERR_INVALID_ARG;
  }
  if (h->state_cb_count.load(std::memory_order_relaxed) == 0) {
    return 1;
  }
  const int64_t age_us = NowUs() - h->last_state_cb_us.load(std::memory_order_relaxed);
  if (age_us > 2000000) {
    return 2;
  }
  if (age_us > 500000) {
    return 3;
  }
  return SDK_OK;
}

float VGetMotorTemp(sdk_handle_t, int) { return 0.0f; }
int VGetMotorMode(sdk_handle_t, int) { return 0; }

int VRequestControl(sdk_handle_t handle, sdk_control_state_t target) {
  auto* h = Cast(handle);
  if (!h) {
    return SDK_ERR_INVALID_ARG;
  }
  if (target == SDK_CONTROL_READY) {
    return RequestReady(h);
  }
  if (target == SDK_CONTROL_UNREADY) {
    return RequestUnready(h);
  }
  return SDK_ERR_INVALID_ARG;
}

sdk_control_state_t VGetControlState(sdk_handle_t handle) {
  auto* h = Cast(handle);
  if (!h) {
    return SDK_CONTROL_UNKNOWN;
  }
  std::string form;
  std::string name;
  if (CheckMotionMode(h, &name, &form) != SDK_OK) {
    return SDK_CONTROL_UNKNOWN;
  }
  return name.empty() ? SDK_CONTROL_READY : SDK_CONTROL_UNREADY;
}

int VSetStateCallback(sdk_handle_t, sdk_vtable_t::sdk_state_callback_t, void*) {
  return SDK_ERR_NOT_SUPPORTED;
}

int VSetJoyCallback(sdk_handle_t, sdk_vtable_t::sdk_joy_callback_t, void*) {
  return SDK_ERR_NOT_SUPPORTED;
}

int VGetDebugStats(sdk_handle_t handle, SdkDebugStats* out) {
  auto* h = Cast(handle);
  if (!h || !out) {
    return SDK_ERR_INVALID_ARG;
  }
  out->state_cb_count = h->state_cb_count.load(std::memory_order_relaxed);
  out->joy_cb_count = h->joy_cb_count.load(std::memory_order_relaxed);
  out->update_count = h->update_count.load(std::memory_order_relaxed);
  out->cmd_count = h->cmd_count.load(std::memory_order_relaxed);
  out->last_state_cb_us = h->last_state_cb_us.load(std::memory_order_relaxed);
  out->last_joy_cb_us = h->last_joy_cb_us.load(std::memory_order_relaxed);
  out->now_us = NowUs();
  return SDK_OK;
}

void* VGetProcAddr(sdk_handle_t, const char* symbol_name) {
  if (symbol_name && std::strcmp(symbol_name, "get_debug_stats") == 0) {
    return reinterpret_cast<void*>(&VGetDebugStats);
  }
  return nullptr;
}

}  // namespace

extern "C" SDK_API const sdk_vtable_t sdk_vtable = {
    sizeof(sdk_vtable_t),
    SDK_API_VERSION,
    VCreate,
    VDestroy,
    VGetNumMotors,
    VGetCapabilities,
    VGetVersion,
    VUpdate,
    VGetMotorQ,
    VGetMotorDq,
    VGetMotorTau,
    VGetMotorQBatch,
    VGetMotorDqBatch,
    VGetMotorTauBatch,
    VBeginCmd,
    VSetMotorCmd,
    VCommitCmd,
    VHasImu,
    VHasJoystick,
    VFalse,
    VFalse,
    VFalse,
    VHasHand,
    VGetImuQuat,
    VGetImuGyro,
    VGetImuRpy,
    VGetImuAcc,
    VZeroFloat,
    VZeroFloat,
    VZeroFloat,
    VNotSupportedFootForce,
    VNotSupportedFootContact,
    VNotSupportedOdom,
    VNotSupportedOdom,
    VNotSupportedHandPos,
    VGetJoyAxes,
    VGetJoyButtons,
    VIsFallen,
    VGetMode,
    VGetCommStatus,
    VGetMotorTemp,
    VGetMotorMode,
    VRequestControl,
    VGetControlState,
    VSetStateCallback,
    VSetJoyCallback,
    VGetProcAddr,
    VHasImu,
};
