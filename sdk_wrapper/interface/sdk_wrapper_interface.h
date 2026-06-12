/**
 * @file sdk_wrapper_interface.h
 * @brief vtable-based SDK wrapper C interface for dlopen dynamic loading
 *
 * All robot-specific SDK wrappers must export exactly one global symbol:
 *   SDK_API extern const sdk_vtable_t sdk_vtable;
 *
 * ABI contract:
 *   - struct_size and api_version sit at fixed offsets (the first 8 bytes) so
 *     the loader can reject wrappers built against an incompatible header
 *   - api_version compatibility is by major version (high 16 bits): the loader
 *     accepts any wrapper whose major matches the runtime's
 *   - within a major version, new vtable entries are only ever appended at the
 *     end; struct_size lets the loader tell which entries a wrapper provides
 *   - SDK_CALL_IF_AVAILABLE / SDK_CALL_OR provide null-pointer safety so a
 *     wrapper can leave optional entries unset
 */

#ifndef SDK_WRAPPER_INTERFACE_H
#define SDK_WRAPPER_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SDK_WRAPPER_BUILDING
#define SDK_API __attribute__((visibility("default")))
#else
#define SDK_API
#endif

// ============================================================================
// Version
// ============================================================================
//
// Packed as 0xMMMMmmmm (major in the high 16 bits, minor in the low 16).
// Compatibility is by major version; a minor bump only appends new vtable
// entries at the end of sdk_vtable_t.
#define SDK_API_VERSION_MAJOR 1
#define SDK_API_VERSION_MINOR 0
#define SDK_API_VERSION ((SDK_API_VERSION_MAJOR << 16) | SDK_API_VERSION_MINOR)

// ============================================================================
// Limits
// ============================================================================

#define SDK_MAX_MOTORS 64
#define SDK_MAX_HAND_JOINTS 16
#define SDK_MAX_FOOT_CHANNELS 16
#define SDK_MAX_JOY_AXES 8
#define SDK_MAX_JOY_BUTTONS 16
#define SDK_MAX_NAME_LEN 64

// ============================================================================
// Error Codes
// ============================================================================

#define SDK_OK 0
#define SDK_ERR_INVALID_ARG (-1)
#define SDK_ERR_NOT_SUPPORTED (-2)
#define SDK_ERR_TIMEOUT (-3)
#define SDK_ERR_COMM (-4)
#define SDK_ERR_INTERNAL (-99)

// ============================================================================
// Capability Flags
// ============================================================================

#define SDK_CAP_LOW_LEVEL 0x0001
#define SDK_CAP_IMU 0x0002  // pelvis/base_link IMU
#define SDK_CAP_BATTERY 0x0004
#define SDK_CAP_FOOT_SENSOR 0x0008
#define SDK_CAP_ODOMETRY 0x0010
#define SDK_CAP_HAND_LEFT 0x0020
#define SDK_CAP_HAND_RIGHT 0x0040
#define SDK_CAP_JOYSTICK 0x0100
#define SDK_CAP_FALL_DETECT 0x0200
#define SDK_CAP_MOTOR_TEMP 0x0400
#define SDK_CAP_MOTOR_MODE 0x0800
#define SDK_CAP_NATIVE_QUAT 0x4000

// ============================================================================
// Control State (vendor-neutral binary view of the motor control authority)
// ============================================================================
//
// Runtime asks one question of every backend: "can I write low-level motor
// commands right now?". The wrapper translates this binary view to whatever
// control modes its own SDK exposes (e.g. a damping/idle mode for UNREADY and
// a low-level command mode for READY).
//
// SDK_CONTROL_UNKNOWN is reported when the wrapper has not observed a state
// yet (e.g. immediately after create() or while a transition is pending and
// the backend has not echoed back).
typedef enum {
  SDK_CONTROL_UNKNOWN = 0,
  SDK_CONTROL_UNREADY = 1,
  SDK_CONTROL_READY = 2,
} sdk_control_state_t;

// ============================================================================
// Opaque Handle
// ============================================================================

typedef struct sdk_handle_s* sdk_handle_t;

// ============================================================================
// Backward-Compat Joystick Constants
// ============================================================================

#define SDK_JOY_AXIS_LX 0
#define SDK_JOY_AXIS_LY 1
#define SDK_JOY_AXIS_RX 2
#define SDK_JOY_AXIS_RY 3

#define SDK_JOY_BTN_A 0
#define SDK_JOY_BTN_B 1
#define SDK_JOY_BTN_X 2
#define SDK_JOY_BTN_Y 3
#define SDK_JOY_BTN_LB 4
#define SDK_JOY_BTN_RB 5
#define SDK_JOY_BTN_BACK 6
#define SDK_JOY_BTN_START 7
#define SDK_JOY_BTN_LT 8
#define SDK_JOY_BTN_RT 9
#define SDK_JOY_BTN_LS 10
#define SDK_JOY_BTN_RS 11
#define SDK_JOY_BTN_DPAD_UP 12
#define SDK_JOY_BTN_DPAD_DOWN 13
#define SDK_JOY_BTN_DPAD_LEFT 14
#define SDK_JOY_BTN_DPAD_RIGHT 15

// ============================================================================
// Calling contract (applies to every vtable function)
// ============================================================================
//
//  - No C++ exception may cross the vtable boundary. Catch internally; a
//    function that throws to the caller is a contract violation.
//  - create()/destroy() are called once each, serially, never concurrently.
//  - Each tick the runtime calls update() first, then getters. update() must
//    be non-blocking (or block only briefly, < ~1ms) and, on success, leave
//    internal state as a consistent snapshot for the following getters.
//  - Command sequence per tick: begin_cmd -> set_motor_cmd x N -> commit_cmd.
//  - Array getters (get_*_batch, get_imu_*, get_foot_*, get_joy_*, ...) follow
//    caller-allocated ownership: the caller passes a buffer + max_count, the
//    wrapper writes and returns the actual count; the buffer may be reused
//    after the call returns. No vtable entry allocates memory for the caller
//    to free (and vice versa).
//  - getters with an out-of-range motor_index (>= get_num_motors()) should
//    return 0 rather than an error.
//  - Soft latency budget: create < 5s, destroy < 2s, update < 500us,
//    getters < 50us, set_motor_cmd/commit_cmd < 200us, request_control < 50us.
//
typedef struct sdk_vtable_t {
  // -- ABI header (fixed position, never move) --
  uint32_t struct_size;
  uint32_t api_version;

  // -- Lifecycle --
  // create(): connect the robot SDK. wrapper_dir is the directory containing
  // the wrapper .so and its config.json. The wrapper reads wrapper_dir/config.json
  // for its own parameters; every key has a hardcoded default so the file is
  // optional. The pointer is valid only for the duration of the call.
  int (*create)(const char* wrapper_dir, sdk_handle_t* out_handle);
  void (*destroy)(sdk_handle_t handle);

  // -- Introspection --
  int (*get_num_motors)(sdk_handle_t handle);
  uint32_t (*get_capabilities)(sdk_handle_t handle);
  const char* (*get_version)(sdk_handle_t handle);

  // -- State snapshot --
  int (*update)(sdk_handle_t handle);

  // -- Motor state getters (single) --
  float (*get_motor_q)(sdk_handle_t handle, int motor_index);
  float (*get_motor_dq)(sdk_handle_t handle, int motor_index);
  float (*get_motor_tau)(sdk_handle_t handle, int motor_index);

  // -- Motor state getters (batch) --
  int (*get_motor_q_batch)(sdk_handle_t handle, float* out, int max_count);
  int (*get_motor_dq_batch)(sdk_handle_t handle, float* out, int max_count);
  int (*get_motor_tau_batch)(sdk_handle_t handle, float* out, int max_count);

  // -- Motor command setters --
  // begin_cmd() MUST zero the internal command buffer (q/dq/tau/kp/kd all 0)
  // so a tick that sets fewer motors cannot leak last frame's command.
  // commit_cmd() returns void; internal errors are logged, not surfaced here.
  void (*begin_cmd)(sdk_handle_t handle);
  void (*set_motor_cmd)(sdk_handle_t handle, int motor_index, float q, float dq, float tau,
                        float kp, float kd);
  void (*commit_cmd)(sdk_handle_t handle);

  // -- Feature queries --
  bool (*has_imu)(sdk_handle_t handle);  // pelvis/base_link IMU
  bool (*has_joystick)(sdk_handle_t handle);
  bool (*has_battery)(sdk_handle_t handle);
  bool (*has_foot_sensor)(sdk_handle_t handle);
  bool (*has_odometry)(sdk_handle_t handle);
  bool (*has_hand)(sdk_handle_t handle, int side);  // side: 0=left, 1=right

  // -- IMU getters (pelvis/base_link IMU) --
  int (*get_imu_quat)(sdk_handle_t handle, float quat[4]);  // [w,x,y,z]
  int (*get_imu_gyro)(sdk_handle_t handle, float gyro[3]);
  int (*get_imu_rpy)(sdk_handle_t handle, float rpy[3]);
  int (*get_imu_acc)(sdk_handle_t handle, float acc[3]);

  // -- Battery getters --
  float (*get_battery_voltage)(sdk_handle_t handle);
  float (*get_battery_current)(sdk_handle_t handle);
  float (*get_battery_percentage)(sdk_handle_t handle);

  // -- Foot sensor getters --
  int (*get_foot_force)(sdk_handle_t handle, int foot, float* forces, int max_channels);
  int (*get_foot_contact)(sdk_handle_t handle, int foot, bool* contacts, int max_channels);

  // -- Odometry getters --
  int (*get_odom_pos)(sdk_handle_t handle, double pos[3]);
  int (*get_odom_vel)(sdk_handle_t handle, double vel[3]);

  // -- Hand getters --
  int (*get_hand_joint_pos)(sdk_handle_t handle, int side, float* pos, int max_joints);

  // -- Joystick getters --
  int (*get_joy_axes)(sdk_handle_t handle, float* axes, int max_axes);
  int (*get_joy_buttons)(sdk_handle_t handle, bool* buttons, int max_buttons);

  // -- Fall / Mode / Diagnostics --
  bool (*is_fallen)(sdk_handle_t handle);
  int (*get_mode)(sdk_handle_t handle);
  int (*get_comm_status)(sdk_handle_t handle);

  // -- Motor extended getters --
  float (*get_motor_temp)(sdk_handle_t handle, int motor_index);
  int (*get_motor_mode)(sdk_handle_t handle, int motor_index);

  // -- Control state --
  //
  // request_control(handle, target):
  //   Submits a transition request to the wrapper. NON-BLOCKING. Returns
  //   SDK_OK once the request has been queued/sent on the wire; the actual
  //   physical transition completes later. Returns SDK_ERR_INVALID_ARG when
  //   target is neither SDK_CONTROL_UNREADY nor SDK_CONTROL_READY, and
  //   SDK_ERR_INTERNAL when the backend rejects the submission outright
  //   (e.g. RPC channel down).
  //
  // get_control_state(handle):
  //   Returns the wrapper's latest view of the backend control state. Some
  //   wrappers issue a synchronous query (lazy RPC) on each call; callers
  //   that poll at high frequency should rate-limit. Returns
  //   SDK_CONTROL_UNKNOWN when the state cannot be determined.
  int (*request_control)(sdk_handle_t handle, sdk_control_state_t target);
  sdk_control_state_t (*get_control_state)(sdk_handle_t handle);

  // -- Callback (optional, for event-driven consumers) --
  // Callbacks fire from a wrapper-internal worker thread (not guaranteed to be
  // the update() thread). The callback body MUST NOT call any vtable function.
  // Ownership of `user` does not transfer to the wrapper.
  typedef void (*sdk_state_callback_t)(sdk_handle_t handle, void* user);
  typedef void (*sdk_joy_callback_t)(sdk_handle_t handle, void* user);
  int (*set_state_callback)(sdk_handle_t handle, sdk_state_callback_t cb, void* user);
  int (*set_joy_callback)(sdk_handle_t handle, sdk_joy_callback_t cb, void* user);

  // -- Extension escape hatch --
  //
  // get_proc_addr(handle, symbol_name) resolves an optional, named function
  // pointer that is not part of this vtable (returns null when the symbol is
  // unknown). Used for backend-private extensions. Wrappers with no extensions
  // may simply return null.
  void* (*get_proc_addr)(sdk_handle_t handle, const char* symbol_name);

  // -- IMU native quaternion query --
  bool (*has_native_quat)(sdk_handle_t handle);
} sdk_vtable_t;

// ============================================================================
// Safety Macros
// ============================================================================

/**
 * Call a vtable function if it is available (non-null and within struct_size).
 * Returns void — use SDK_CALL_OR for functions that return a value.
 */
#define SDK_CALL_IF_AVAILABLE(vt, func, ...)                                                       \
  do {                                                                                             \
    if ((vt) && offsetof(sdk_vtable_t, func) + sizeof(void*) <= (vt)->struct_size && (vt)->func) { \
      (vt)->func(__VA_ARGS__);                                                                     \
    }                                                                                              \
  } while (0)

/**
 * Call a vtable function if available, otherwise return a fallback value.
 */
#define SDK_CALL_OR(vt, func, fallback, ...)                                                 \
  (((vt) && offsetof(sdk_vtable_t, func) + sizeof(void*) <= (vt)->struct_size && (vt)->func) \
       ? (vt)->func(__VA_ARGS__)                                                             \
       : (fallback))

#ifdef __cplusplus
}
#endif

#endif  // SDK_WRAPPER_INTERFACE_H
