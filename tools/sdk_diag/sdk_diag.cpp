/**
 * @file sdk_diag.cpp
 * @brief Standalone SDK wrapper diagnostic tool
 *
 * Loads an SDK wrapper .so directly via dlopen (no iceoryx proxy) and provides
 * interactive commands to inspect hardware state and send safe motor commands.
 *
 * Safety: all motor commands are clamped to configurable torque/gain limits.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "manifest.h"
#include "package_check.h"
#include "sdk_wrapper/interface/sdk_wrapper_interface.h"

// =============================================================================
// Safety limits — hard-coded maximums that cannot be overridden
// =============================================================================

// =============================================================================
// Debug stats structure (must match the one in booster_t1_wrapper / wrappers)
// =============================================================================

struct SdkDebugStats {
  uint64_t state_cb_count;
  uint64_t joy_cb_count;
  uint64_t update_count;
  uint64_t cmd_count;
  int64_t last_state_cb_us;
  int64_t last_joy_cb_us;
  int64_t now_us;
};

using SdkGetDebugStatsFn = int (*)(sdk_handle_t, SdkDebugStats*);

static constexpr float kMaxTorque = 5.0f;
static constexpr float kMaxKp = 30.0f;
static constexpr float kMaxKd = 5.0f;
static constexpr float kDampingKd = 0.1f;
static constexpr int kDampingFrames = 50;
static constexpr int kDefaultReadCount = 10;
static constexpr int kReadIntervalMs = 100;

static std::atomic<bool> g_running{true};

static void OnSignal(int) {
  g_running.store(false);
}

// =============================================================================
// CLI argument parsing
// =============================================================================

static void PrintUsage(const char* prog) {
  fprintf(stderr,
          "Usage:\n"
          "  %s --check [--lang zh|en] <package_dir|archive>   Run static package checks\n"
          "  %s <package_dir|archive>                          Interactive diagnostic mode\n"
          "\n"
          "  <package_dir|archive>  Directory or .tar / .tar.gz / .tgz / .zip\n"
          "  --lang zh|en           --check report language (default zh; or $SDK_DIAG_LANG)\n"
          "  -h, --help             Show this help\n",
          prog, prog);
}


// =============================================================================
// SDK loading (same pattern as sdk_worker)
// =============================================================================

struct SdkSession {
  void* dl_handle{nullptr};
  const sdk_vtable_t* vt{nullptr};
  sdk_handle_t handle{nullptr};
  int num_motors{0};
};

static bool LoadSdk(const std::string& package_dir, SdkSession& session) {
  // Read manifest.yaml to find the entrypoint .so filename
  std::string manifest_path = package_dir + "/manifest.yaml";
  auto parse_result = sdk_diag::ParseYamlFile(manifest_path);
  if (!parse_result.ok) {
    fprintf(stderr, "Failed to parse manifest.yaml: %s\n", parse_result.error.c_str());
    return false;
  }

  sdk_diag::WrapperManifest manifest;
  std::vector<std::string> errors;
  sdk_diag::ParseManifest(parse_result.root, manifest, errors);
  if (manifest.api_entrypoint.empty()) {
    fprintf(stderr, "manifest.yaml missing api.entrypoint\n");
    return false;
  }

  std::string so_path = package_dir + "/" + manifest.api_entrypoint;

  session.dl_handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!session.dl_handle) {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return false;
  }

  session.vt = static_cast<const sdk_vtable_t*>(dlsym(session.dl_handle, "sdk_vtable"));
  if (!session.vt) {
    fprintf(stderr, "dlsym(sdk_vtable) failed: %s\n", dlerror());
    dlclose(session.dl_handle);
    return false;
  }

  if ((session.vt->api_version >> 16) != (SDK_API_VERSION >> 16)) {
    fprintf(stderr, "API major version mismatch: 0x%08x vs 0x%08x\n", session.vt->api_version,
            SDK_API_VERSION);
    dlclose(session.dl_handle);
    return false;
  }

  int ret = SDK_OK;
  try {
    ret = session.vt->create(package_dir.c_str(), &session.handle);
  } catch (const std::exception& e) {
    fprintf(stderr, "vtable->create() threw exception: %s\n", e.what());
    dlclose(session.dl_handle);
    return false;
  }

  if (ret != SDK_OK || !session.handle) {
    fprintf(stderr, "vtable->create() failed (ret=%d)\n", ret);
    dlclose(session.dl_handle);
    return false;
  }

  session.num_motors = session.vt->get_num_motors(session.handle);
  return true;
}

static void CloseSdk(SdkSession& session) {
  if (session.vt && session.handle && session.vt->destroy) {
    session.vt->destroy(session.handle);
  }
  if (session.dl_handle) {
    dlclose(session.dl_handle);
  }
  session = {};
}

// =============================================================================
// Formatting helpers
// =============================================================================

static std::string CapString(uint32_t caps) {
  std::string result;
  auto add = [&](uint32_t flag, const char* name) {
    if (caps & flag) {
      if (!result.empty())
        result += " | ";
      result += name;
    }
  };
  add(SDK_CAP_LOW_LEVEL, "LOW_LEVEL");
  add(SDK_CAP_IMU, "IMU");
  add(SDK_CAP_BATTERY, "BATTERY");
  add(SDK_CAP_FOOT_SENSOR, "FOOT_SENSOR");
  add(SDK_CAP_ODOMETRY, "ODOMETRY");
  add(SDK_CAP_HAND_LEFT, "HAND_LEFT");
  add(SDK_CAP_HAND_RIGHT, "HAND_RIGHT");
  add(SDK_CAP_JOYSTICK, "JOYSTICK");
  add(SDK_CAP_FALL_DETECT, "FALL_DETECT");
  add(SDK_CAP_MOTOR_TEMP, "MOTOR_TEMP");
  add(SDK_CAP_MOTOR_MODE, "MOTOR_MODE");
  add(SDK_CAP_NATIVE_QUAT, "NATIVE_QUAT");
  return result.empty() ? "none" : result;
}

static void PrintSeparator() {
  printf("────────────────────────────────────────────────────\n");
}

// =============================================================================
// Commands
// =============================================================================

static const char* ControlStateName(sdk_control_state_t s);

static void CmdInfo(SdkSession& s) {
  auto* vt = s.vt;
  auto h = s.handle;

  const char* version = SDK_CALL_OR(vt, get_version, "unknown", h);
  uint32_t caps = SDK_CALL_OR(vt, get_capabilities, 0u, h);
  int comm = SDK_CALL_OR(vt, get_comm_status, -1, h);
  int mode = SDK_CALL_OR(vt, get_mode, -1, h);
  bool fallen = SDK_CALL_OR(vt, is_fallen, false, h);

  PrintSeparator();
  printf("SDK Wrapper Info\n");
  PrintSeparator();
  printf("  Version:      %s\n", version);
  printf("  API version:  0x%08x\n", vt->api_version);
  printf("  Struct size:  %u bytes\n", vt->struct_size);
  printf("  Num motors:   %d\n", s.num_motors);
  printf("  Capabilities: 0x%04x (%s)\n", caps, CapString(caps).c_str());
  const char* comm_str = "unknown";
  switch (comm) {
    case 0:
      comm_str = "OK";
      break;
    case 1:
      comm_str = "NO_DATA (no DDS callbacks received)";
      break;
    case 2:
      comm_str = "TIMEOUT (no data for >2s)";
      break;
    case 3:
      comm_str = "STALE (no data for >500ms)";
      break;
    case -1:
      comm_str = "N/A (function missing)";
      break;
    case -2:
      comm_str = "NOT_SUPPORTED (wrapper too old)";
      break;
    default:
      break;
  }
  printf("  Comm status:  %d (%s)\n", comm, comm_str);
  printf("  Mode:         %d\n", mode);
  printf("  Fallen:       %s\n", fallen ? "YES" : "no");
  printf("  Has IMU:      %s\n", SDK_CALL_OR(vt, has_imu, false, h) ? "yes" : "no");
  printf("  Has joystick: %s\n", SDK_CALL_OR(vt, has_joystick, false, h) ? "yes" : "no");
  printf("  Has battery:  %s\n", SDK_CALL_OR(vt, has_battery, false, h) ? "yes" : "no");
  printf("  Has foot:     %s\n", SDK_CALL_OR(vt, has_foot_sensor, false, h) ? "yes" : "no");
  printf("  Native quat:  %s\n", SDK_CALL_OR(vt, has_native_quat, false, h) ? "yes" : "no");
  PrintSeparator();
}

static bool UpdateOnce(SdkSession& s) {
  int ret = s.vt->update(s.handle);
  if (ret != SDK_OK) {
    fprintf(stderr, "  update() returned %d\n", ret);
    return false;
  }
  return true;
}

static bool EnsureControlReady(SdkSession& s, const char* cmd_name) {
  if (!s.vt->get_control_state) {
    return true;
  }
  sdk_control_state_t cur = s.vt->get_control_state(s.handle);
  if (cur == SDK_CONTROL_READY) {
    return true;
  }
  printf("  %s skipped: control state is %s. Run `ready` first.\n", cmd_name,
         ControlStateName(cur));
  return false;
}

static void PrintMotorState(SdkSession& s) {
  auto* vt = s.vt;
  auto h = s.handle;
  int n = s.num_motors;

  printf("  %-5s  %10s  %10s  %10s", "Motor", "q (rad)", "dq (rad/s)", "tau (Nm)");
  bool has_temp = SDK_CALL_OR(vt, get_motor_temp, -999.0f, h, 0) > -999.0f;
  bool has_mode = SDK_CALL_OR(vt, get_motor_mode, -999, h, 0) > -999;
  if (has_temp)
    printf("  %8s", "temp(C)");
  if (has_mode)
    printf("  %6s", "mode");
  printf("\n");

  for (int i = 0; i < n && i < SDK_MAX_MOTORS; ++i) {
    float q = vt->get_motor_q(h, i);
    float dq = vt->get_motor_dq(h, i);
    float tau = SDK_CALL_OR(vt, get_motor_tau, 0.0f, h, i);
    printf("  [%2d]   %10.4f  %10.4f  %10.4f", i, q, dq, tau);
    if (has_temp)
      printf("  %8.1f", SDK_CALL_OR(vt, get_motor_temp, 0.0f, h, i));
    if (has_mode)
      printf("  %6d", SDK_CALL_OR(vt, get_motor_mode, 0, h, i));
    printf("\n");
  }
}

static void CmdRead(SdkSession& s, int count) {
  for (int round = 0; round < count && g_running.load(); ++round) {
    if (!UpdateOnce(s))
      break;
    printf("\n--- Read #%d/%d ---\n", round + 1, count);
    PrintMotorState(s);
    if (round + 1 < count) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kReadIntervalMs));
    }
  }
}

static void CmdImu(SdkSession& s, int count) {
  auto* vt = s.vt;
  auto h = s.handle;

  if (!SDK_CALL_OR(vt, has_imu, false, h)) {
    printf("  IMU not available on this wrapper\n");
    return;
  }

  for (int round = 0; round < count && g_running.load(); ++round) {
    if (!UpdateOnce(s))
      break;

    float rpy[3] = {}, gyro[3] = {}, acc[3] = {}, quat[4] = {};
    SDK_CALL_IF_AVAILABLE(vt, get_imu_rpy, h, rpy);
    SDK_CALL_IF_AVAILABLE(vt, get_imu_gyro, h, gyro);
    SDK_CALL_IF_AVAILABLE(vt, get_imu_acc, h, acc);
    int quat_ret = SDK_CALL_OR(vt, get_imu_quat, -1, h, quat);

    printf("\n--- IMU #%d/%d ---\n", round + 1, count);
    printf("  RPY:  [%.4f, %.4f, %.4f] rad\n", rpy[0], rpy[1], rpy[2]);
    printf("  Gyro: [%.4f, %.4f, %.4f] rad/s\n", gyro[0], gyro[1], gyro[2]);
    printf("  Acc:  [%.4f, %.4f, %.4f] m/s^2\n", acc[0], acc[1], acc[2]);
    if (quat_ret == SDK_OK) {
      printf("  Quat: [%.4f, %.4f, %.4f, %.4f] (w,x,y,z)\n", quat[0], quat[1], quat[2], quat[3]);
    }

    if (round + 1 < count) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kReadIntervalMs));
    }
  }
}

static void CmdBattery(SdkSession& s) {
  auto* vt = s.vt;
  auto h = s.handle;

  if (!SDK_CALL_OR(vt, has_battery, false, h)) {
    printf("  Battery not available on this wrapper\n");
    return;
  }
  if (!UpdateOnce(s))
    return;

  printf("  Voltage:    %.2f V\n", SDK_CALL_OR(vt, get_battery_voltage, 0.0f, h));
  printf("  Current:    %.2f A\n", SDK_CALL_OR(vt, get_battery_current, 0.0f, h));
  printf("  Percentage: %.1f%%\n", SDK_CALL_OR(vt, get_battery_percentage, 0.0f, h));
}

static void CmdJoystick(SdkSession& s) {
  auto* vt = s.vt;
  auto h = s.handle;

  if (!SDK_CALL_OR(vt, has_joystick, false, h)) {
    printf("  Joystick not available on this wrapper\n");
    return;
  }
  if (!UpdateOnce(s))
    return;

  float axes[SDK_MAX_JOY_AXES] = {};
  bool buttons[SDK_MAX_JOY_BUTTONS] = {};
  int na = SDK_CALL_OR(vt, get_joy_axes, 0, h, axes, SDK_MAX_JOY_AXES);
  int nb = SDK_CALL_OR(vt, get_joy_buttons, 0, h, buttons, SDK_MAX_JOY_BUTTONS);

  printf("  Axes (%d):", na);
  for (int i = 0; i < na; ++i)
    printf(" %.3f", axes[i]);
  printf("\n  Buttons (%d):", nb);
  for (int i = 0; i < nb; ++i)
    printf(" %d", buttons[i] ? 1 : 0);
  printf("\n");
}

static void SendDamping(SdkSession& s, float kd, int frames) {
  auto* vt = s.vt;
  auto h = s.handle;
  float clamped_kd = std::min(kd, kMaxKd);

  printf("  Sending %d damping frames (kd=%.3f)...\n", frames, clamped_kd);
  for (int f = 0; f < frames && g_running.load(); ++f) {
    SDK_CALL_IF_AVAILABLE(vt, begin_cmd, h);
    for (int i = 0; i < s.num_motors && i < SDK_MAX_MOTORS; ++i) {
      SDK_CALL_IF_AVAILABLE(vt, set_motor_cmd, h, i, 0.0f, 0.0f, 0.0f, 0.0f, clamped_kd);
    }
    SDK_CALL_IF_AVAILABLE(vt, commit_cmd, h);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  printf("  Done.\n");
}

static void CmdDamping(SdkSession& s) {
  if (!EnsureControlReady(s, "damping"))
    return;
  SendDamping(s, kDampingKd, kDampingFrames);
}

static void CmdZero(SdkSession& s, int frames) {
  auto* vt = s.vt;
  auto h = s.handle;

  if (!EnsureControlReady(s, "zero"))
    return;

  printf("  Sending %d zero-torque frames...\n", frames);
  for (int f = 0; f < frames && g_running.load(); ++f) {
    SDK_CALL_IF_AVAILABLE(vt, begin_cmd, h);
    for (int i = 0; i < s.num_motors && i < SDK_MAX_MOTORS; ++i) {
      SDK_CALL_IF_AVAILABLE(vt, set_motor_cmd, h, i, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    SDK_CALL_IF_AVAILABLE(vt, commit_cmd, h);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  printf("  Done.\n");
}

static void CmdHold(SdkSession& s, float kp, float kd, int duration_ms) {
  auto* vt = s.vt;
  auto h = s.handle;
  float clamped_kp = std::min(kp, kMaxKp);
  float clamped_kd = std::min(kd, kMaxKd);

  if (!EnsureControlReady(s, "hold"))
    return;
  if (!UpdateOnce(s))
    return;

  std::vector<float> hold_pos(s.num_motors, 0.0f);
  for (int i = 0; i < s.num_motors && i < SDK_MAX_MOTORS; ++i) {
    hold_pos[i] = vt->get_motor_q(h, i);
  }

  printf("  Holding current positions for %dms (kp=%.1f, kd=%.3f)...\n", duration_ms, clamped_kp,
         clamped_kd);
  printf("  Hold positions:");
  for (int i = 0; i < s.num_motors; ++i)
    printf(" %.3f", hold_pos[i]);
  printf("\n");

  auto start = std::chrono::steady_clock::now();
  while (g_running.load()) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (elapsed >= duration_ms)
      break;

    vt->update(h);
    SDK_CALL_IF_AVAILABLE(vt, begin_cmd, h);
    for (int i = 0; i < s.num_motors && i < SDK_MAX_MOTORS; ++i) {
      SDK_CALL_IF_AVAILABLE(vt, set_motor_cmd, h, i, hold_pos[i], 0.0f, 0.0f, clamped_kp,
                            clamped_kd);
    }
    SDK_CALL_IF_AVAILABLE(vt, commit_cmd, h);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  printf("  Hold complete, sending damping...\n");
  SendDamping(s, kDampingKd, 20);
}

static const char* RequestControlResultString(int rc) {
  switch (rc) {
    case SDK_OK:
      return "OK";
    case SDK_ERR_INVALID_ARG:
      return "INVALID_ARG (target not recognised)";
    case SDK_ERR_NOT_SUPPORTED:
      return "NOT_SUPPORTED (wrapper has no request_control)";
    case SDK_ERR_TIMEOUT:
      return "TIMEOUT (backend did not confirm request)";
    case SDK_ERR_COMM:
      return "COMM (backend communication failed)";
    case SDK_ERR_INTERNAL:
      return "INTERNAL (backend rejected request, check robot state)";
    default:
      return "unknown";
  }
}

static const char* ControlStateName(sdk_control_state_t s) {
  switch (s) {
    case SDK_CONTROL_UNREADY:
      return "UNREADY";
    case SDK_CONTROL_READY:
      return "READY";
    case SDK_CONTROL_UNKNOWN:
    default:
      return "UNKNOWN";
  }
}

static void CmdRequestControl(SdkSession& s, sdk_control_state_t target, const char* label) {
  int rc = SDK_CALL_OR(s.vt, request_control, SDK_ERR_NOT_SUPPORTED, s.handle, target);
  printf("  request_control(%s) submitted -> %d (%s)\n", label, rc, RequestControlResultString(rc));
  if (rc != SDK_OK) {
    return;
  }
  // Poll get_control_state for a short window so the operator gets feedback.
  constexpr int kPollDeadlineMs = 5000;
  constexpr int kPollIntervalMs = 100;
  using clock = std::chrono::steady_clock;
  auto deadline = clock::now() + std::chrono::milliseconds(kPollDeadlineMs);
  while (clock::now() < deadline) {
    sdk_control_state_t cur =
        SDK_CALL_OR(s.vt, get_control_state, SDK_CONTROL_UNKNOWN, s.handle);
    if (cur == target) {
      printf("  control state confirmed: %s\n", ControlStateName(cur));
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  }
  sdk_control_state_t final_state =
      SDK_CALL_OR(s.vt, get_control_state, SDK_CONTROL_UNKNOWN, s.handle);
  printf("  timed out waiting for %s; current state: %s\n", label, ControlStateName(final_state));
}

static void CmdDiag(SdkSession& s) {
  auto* vt = s.vt;
  auto h = s.handle;

  SdkGetDebugStatsFn get_stats = nullptr;
  if (vt->get_proc_addr) {
    get_stats = reinterpret_cast<SdkGetDebugStatsFn>(vt->get_proc_addr(h, "get_debug_stats"));
  }
  if (!get_stats) {
    printf("  debug stats not available (wrapper does not export get_debug_stats)\n");
    return;
  }

  SdkDebugStats stats{};
  int ret = get_stats(h, &stats);
  if (ret != SDK_OK) {
    printf("  get_debug_stats() returned %d\n", ret);
    return;
  }

  PrintSeparator();
  printf("DDS Diagnostic Stats\n");
  PrintSeparator();
  printf("  LowState callbacks:  %lu\n", (unsigned long)stats.state_cb_count);
  printf("  Joystick callbacks:  %lu\n", (unsigned long)stats.joy_cb_count);
  printf("  update() calls:      %lu\n", (unsigned long)stats.update_count);
  printf("  commit_cmd() calls:  %lu\n", (unsigned long)stats.cmd_count);

  if (stats.state_cb_count > 0) {
    int64_t age_ms = (stats.now_us - stats.last_state_cb_us) / 1000;
    printf("  Last LowState age:   %ld ms ago\n", (long)age_ms);
  } else {
    printf("  Last LowState age:   NEVER received\n");
  }

  if (stats.joy_cb_count > 0) {
    int64_t age_ms = (stats.now_us - stats.last_joy_cb_us) / 1000;
    printf("  Last Joystick age:   %ld ms ago\n", (long)age_ms);
  } else {
    printf("  Last Joystick age:   NEVER received\n");
  }
  PrintSeparator();

  int comm = SDK_CALL_OR(vt, get_comm_status, -1, h);
  printf("  Comm status:         %d", comm);
  switch (comm) {
    case 0:
      printf(" (OK)\n");
      break;
    case 1:
      printf(" (NO_DATA)\n");
      break;
    case 2:
      printf(" (TIMEOUT >2s)\n");
      break;
    case 3:
      printf(" (STALE >500ms)\n");
      break;
    default:
      printf("\n");
      break;
  }
}

static void CmdWait(SdkSession& s, int timeout_sec) {
  auto* vt = s.vt;
  auto h = s.handle;

  SdkGetDebugStatsFn get_stats = nullptr;
  if (vt->get_proc_addr) {
    get_stats = reinterpret_cast<SdkGetDebugStatsFn>(vt->get_proc_addr(h, "get_debug_stats"));
  }
  if (!get_stats) {
    printf("  debug stats not available, falling back to update polling\n");
    printf("  Waiting up to %ds for non-zero motor data...\n", timeout_sec);
    auto start = std::chrono::steady_clock::now();
    while (g_running.load()) {
      auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed >= timeout_sec) {
        printf("  TIMEOUT after %ds -- no data received\n", timeout_sec);
        return;
      }
      vt->update(h);
      bool any_nonzero = false;
      for (int i = 0; i < s.num_motors && i < SDK_MAX_MOTORS; ++i) {
        if (vt->get_motor_q(h, i) != 0.0f) {
          any_nonzero = true;
          break;
        }
      }
      if (any_nonzero) {
        printf("  Data received after %lds!\n", (long)elapsed);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return;
  }

  printf("  Waiting up to %ds for first DDS LowState callback...\n", timeout_sec);
  auto start = std::chrono::steady_clock::now();
  uint64_t last_count = 0;
  while (g_running.load()) {
    auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start)
            .count();
    if (elapsed >= timeout_sec) {
      printf("  TIMEOUT after %ds -- no LowState callback received\n", timeout_sec);
      printf("  Possible causes:\n");
      printf("    - Robot not powered on or not in low-level mode\n");
      printf("    - Wrong network interface (check -n flag)\n");
      printf("    - IP not on same subnet as robot\n");
      printf("    - DDS domain ID mismatch (check -d flag)\n");
      printf("    - Firewall blocking UDP multicast\n");
      return;
    }

    SdkDebugStats stats{};
    get_stats(h, &stats);
    if (stats.state_cb_count > last_count) {
      printf("  First LowState received after %lds! (total callbacks: %lu)\n", (long)elapsed,
             (unsigned long)stats.state_cb_count);
      return;
    }

    if (elapsed > 0 && elapsed % 5 == 0 && stats.state_cb_count == last_count) {
      printf("  ... still waiting (%lds, state_cb=%lu, joy_cb=%lu)\n", (long)elapsed,
             (unsigned long)stats.state_cb_count, (unsigned long)stats.joy_cb_count);
    }
    last_count = stats.state_cb_count;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

static void PrintHelp() {
  PrintSeparator();
  printf("SDK Diagnostic Commands\n");
  PrintSeparator();
  printf("  info                      Show wrapper info and capabilities\n");
  printf("  read [N]                  Read motor state N times (default %d)\n", kDefaultReadCount);
  printf("  imu [N]                   Read IMU data N times (default %d)\n", kDefaultReadCount);
  printf("  battery                   Read battery status\n");
  printf("  joystick                  Read joystick state\n");
  printf("  damping                   Send damping command (kd=%.2f, %d frames)\n", kDampingKd,
         kDampingFrames);
  printf("  zero [N]                  Send N zero-torque frames (default %d)\n", kDampingFrames);
  printf("  hold [kp] [kd] [ms]      Hold current position (default kp=10 kd=0.5 ms=3000)\n");
  printf("  help                      Show this help\n");
  printf("  diag                      Show DDS diagnostic stats\n");
  printf("  wait [sec]                Wait for first DDS data (default 30s)\n");
  printf("  unready                   request_control -> UNREADY (release motors, e.g. damping)\n");
  printf("  ready                     request_control -> READY (low-level motor cmd accepted)\n");
  printf("  state                     show current control state\n");
  printf("  quit / exit               Exit\n");
  PrintSeparator();
  printf("Safety limits: max_tau=%.1f Nm, max_kp=%.1f, max_kd=%.1f\n", kMaxTorque, kMaxKp, kMaxKd);
  PrintSeparator();
}

// =============================================================================
// REPL
// =============================================================================

static std::vector<std::string> Tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream iss(line);
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

static void RunRepl(SdkSession& s) {
  PrintHelp();

  std::string line;
  while (g_running.load()) {
    printf("\nsdk_diag> ");
    fflush(stdout);
    if (!std::getline(std::cin, line))
      break;

    auto tokens = Tokenize(line);
    if (tokens.empty())
      continue;

    const std::string& cmd = tokens[0];

    if (cmd == "quit" || cmd == "exit" || cmd == "q") {
      break;
    } else if (cmd == "help" || cmd == "h" || cmd == "?") {
      PrintHelp();
    } else if (cmd == "info" || cmd == "i") {
      CmdInfo(s);
    } else if (cmd == "read" || cmd == "r") {
      int count = (tokens.size() > 1) ? std::atoi(tokens[1].c_str()) : kDefaultReadCount;
      CmdRead(s, std::max(1, count));
    } else if (cmd == "imu") {
      int count = (tokens.size() > 1) ? std::atoi(tokens[1].c_str()) : kDefaultReadCount;
      CmdImu(s, std::max(1, count));
    } else if (cmd == "battery" || cmd == "bat") {
      CmdBattery(s);
    } else if (cmd == "joystick" || cmd == "joy") {
      CmdJoystick(s);
    } else if (cmd == "damping" || cmd == "damp") {
      CmdDamping(s);
    } else if (cmd == "zero") {
      int frames = (tokens.size() > 1) ? std::atoi(tokens[1].c_str()) : kDampingFrames;
      CmdZero(s, std::max(1, frames));
    } else if (cmd == "hold") {
      float kp = (tokens.size() > 1) ? std::atof(tokens[1].c_str()) : 10.0f;
      float kd = (tokens.size() > 2) ? std::atof(tokens[2].c_str()) : 0.5f;
      int ms = (tokens.size() > 3) ? std::atoi(tokens[3].c_str()) : 3000;
      CmdHold(s, kp, kd, std::max(100, ms));
    } else if (cmd == "diag" || cmd == "d") {
      CmdDiag(s);
    } else if (cmd == "wait" || cmd == "w") {
      int timeout = (tokens.size() > 1) ? std::atoi(tokens[1].c_str()) : 30;
      CmdWait(s, std::max(5, timeout));
    } else if (cmd == "unready" || cmd == "release") {
      CmdRequestControl(s, SDK_CONTROL_UNREADY, "UNREADY");
    } else if (cmd == "ready") {
      CmdRequestControl(s, SDK_CONTROL_READY, "READY");
    } else if (cmd == "state") {
      sdk_control_state_t cur =
          SDK_CALL_OR(s.vt, get_control_state, SDK_CONTROL_UNKNOWN, s.handle);
      printf("  control state: %s\n", ControlStateName(cur));
    } else {
      printf("  Unknown command: '%s'. Type 'help' for usage.\n", cmd.c_str());
    }
  }
}

// =============================================================================
// --check mode (static package checks)
// =============================================================================

// Resolve the report language: CLI flag wins, else SDK_DIAG_LANG env, else zh.
static bool ParseLang(const std::string& v, sdk_diag::Lang& out) {
  if (v == "zh" || v == "cn" || v == "zh-CN" || v == "zh_CN" || v == "chinese") {
    out = sdk_diag::Lang::Zh;
    return true;
  }
  if (v == "en" || v == "en-US" || v == "en_US" || v == "english") {
    out = sdk_diag::Lang::En;
    return true;
  }
  return false;
}

static sdk_diag::Lang DefaultLangFromEnv() {
  sdk_diag::Lang lang = sdk_diag::Lang::Zh;
  if (const char* e = std::getenv("SDK_DIAG_LANG")) {
    ParseLang(e, lang);
  }
  return lang;
}

static const char* kCheckUsage =
    "Usage: sdk_diag --check [--lang zh|en] <package_dir|archive>\n"
    "  --lang zh|en   Report language (default zh, also honours $SDK_DIAG_LANG)\n"
    "  --zh / --en    Shorthand for --lang zh / --lang en\n";

static int RunCheckCli(int argc, char* argv[]) {
  std::string pkg;
  sdk_diag::Lang lang = DefaultLangFromEnv();
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      fprintf(stderr, "%s", kCheckUsage);
      return 2;
    }
    if (a == "--zh") {
      lang = sdk_diag::Lang::Zh;
      continue;
    }
    if (a == "--en") {
      lang = sdk_diag::Lang::En;
      continue;
    }
    if (a == "--lang" || a == "-l") {
      if (i + 1 >= argc) {
        fprintf(stderr, "sdk_diag --check: %s requires an argument (zh|en)\n", a.c_str());
        return 2;
      }
      if (!ParseLang(argv[++i], lang)) {
        fprintf(stderr, "sdk_diag --check: invalid --lang value '%s' (expected zh|en)\n", argv[i]);
        return 2;
      }
      continue;
    }
    if (a.rfind("--lang=", 0) == 0) {
      if (!ParseLang(a.substr(7), lang)) {
        fprintf(stderr, "sdk_diag --check: invalid --lang value '%s' (expected zh|en)\n",
                a.substr(7).c_str());
        return 2;
      }
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      fprintf(stderr, "sdk_diag --check: unknown option %s\n", a.c_str());
      return 2;
    }
    if (pkg.empty()) pkg = a;
  }
  if (pkg.empty()) {
    fprintf(stderr, "%s", kCheckUsage);
    return 2;
  }

  std::string cleanup_dir;
  std::string dir = sdk_diag::ResolvePackageDir(pkg, cleanup_dir, lang);
  if (dir.empty()) return 2;

  int rc = sdk_diag::RunPackageCheck(dir, lang);

  if (!cleanup_dir.empty()) {
    std::string rm = "rm -rf '" + cleanup_dir + "'";
    (void)std::system(rm.c_str());
  }
  return rc;
}

// =============================================================================
// main
// =============================================================================

// =============================================================================
// LD_LIBRARY_PATH bootstrap
// =============================================================================
//
// ROS 2's C typesupport dispatch (rosidl_typesupport_c) loads the concrete
// <pkg>__rosidl_typesupport_*_c libraries by bare name through the dynamic
// linker search path at runtime. The wrapper bundles them in <pkg>/lib, but its
// $ORIGIN/lib RPATH does not apply to that dlopen, and glibc caches
// LD_LIBRARY_PATH at startup so setting it from this process would not take
// effect. Re-exec ourselves once with the bundled lib/ dir prepended so the
// child's dynamic linker picks it up at init.
//
// Only handles the package-directory case (argv[1] is a dir). Archives are
// resolved later by ResolvePackageDir; --check is static and needs no wrapper.
static void ReexecWithBundledLibPath(int argc, char* argv[]) {
  (void)argc;
  if (std::getenv("SDK_DIAG_LDPATH_SET") != nullptr) {
    return;
  }
  if (!argv[1] || argv[1][0] == '-') {
    return;
  }
  std::string lib_dir = std::string(argv[1]) + "/lib";
  struct stat st {};
  if (stat(lib_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    return;
  }
  const char* cur = std::getenv("LD_LIBRARY_PATH");
  std::string next = (cur && *cur) ? lib_dir + ":" + cur : lib_dir;
  setenv("LD_LIBRARY_PATH", next.c_str(), 1);
  setenv("SDK_DIAG_LDPATH_SET", "1", 1);
  execv("/proc/self/exe", argv);
  fprintf(stderr, "sdk_diag: re-exec failed: %s (continuing without bundled lib path)\n",
          strerror(errno));
}

int main(int argc, char* argv[]) {
  ReexecWithBundledLibPath(argc, argv);

  // -h/--help anywhere in argv prints full help for both modes
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
  }

  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  // --check mode
  if (std::strcmp(argv[1], "--check") == 0) {
    return RunCheckCli(argc, argv);
  }

  // Unknown options
  if (argv[1][0] == '-') {
    fprintf(stderr, "Unknown option: %s\n", argv[1]);
    PrintUsage(argv[0]);
    return 1;
  }

  // Interactive mode: argv[1] is package_dir or archive
  std::string pkg = argv[1];
  std::string cleanup_dir;
  std::string package_dir = sdk_diag::ResolvePackageDir(pkg, cleanup_dir);
  if (package_dir.empty()) {
    return 1;
  }

  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);

  printf("=== SDK Diagnostic Tool ===\n");
  printf("Loading package: %s\n", package_dir.c_str());

  SdkSession session;
  if (!LoadSdk(package_dir, session)) {
    if (!cleanup_dir.empty()) {
      std::string rm = "rm -rf '" + cleanup_dir + "'";
      (void)std::system(rm.c_str());
    }
    return 1;
  }

  printf("SDK loaded OK (motors=%d)\n", session.num_motors);

  // Initial update + info
  if (UpdateOnce(session)) {
    CmdInfo(session);
  }

  RunRepl(session);

  // Graceful shutdown: send damping
  printf("\nShutting down, sending damping...\n");
  SendDamping(session, kDampingKd, 10);

  CloseSdk(session);

  if (!cleanup_dir.empty()) {
    std::string rm = "rm -rf '" + cleanup_dir + "'";
    (void)std::system(rm.c_str());
  }

  printf("Bye.\n");
  return 0;
}
