# Unitree G1 RMW wrapper

This wrapper exports the standard `sdk_vtable` ABI and talks to Unitree G1 ROS 2
topics through the ROS 2 C client layer over the selected RMW implementation. It
intentionally does not link `rclcpp` or use a ROS executor.

Build prerequisites:

```bash
source /opt/ros/humble/setup.bash
cd sdk/unitree_ros2/cyclonedds_ws
colcon build --packages-select unitree_hg unitree_api --cmake-args -DBUILD_TESTING=OFF
```

Build the wrapper:

```bash
./scripts/build_sdk_wrappers.sh --local g1-rmw
```

## CI build

Push a tag matching `sdk-g1-rmw-v<major>.<minor>.<patch>`:

```bash
git tag sdk-g1-rmw-v0.1.0
git push origin sdk-g1-rmw-v0.1.0
```

Two CI jobs run in parallel (`build:sdk-g1-rmw:aarch64:foxy` and
`build:sdk-g1-rmw:aarch64:humble`), each using the corresponding `ros:*`
image, building `unitree_hg` + `unitree_api` via colcon, then compiling
the wrapper and running `ci/package_sdk.sh`. The release job waits for
both builds and attaches the resulting tarballs to the GitLab release.

### Package structure

The published tarballs (`sdk-unitree-g1-rmw-aarch64-foxy.tar.gz` and
`sdk-unitree-g1-rmw-aarch64-humble.tar.gz`) each contain:

```
libunitree_g1_rmw_wrapper.so    # entrypoint
config.json                     # runtime configuration
lib/
  libunitree_hg__rosidl_generator_c.so
  libunitree_hg__rosidl_typesupport_c.so
  libunitree_hg__rosidl_typesupport_introspection_c.so
  libunitree_hg__rosidl_typesupport_fastrtps_c.so
  libunitree_api__rosidl_generator_c.so
  libunitree_api__rosidl_typesupport_c.so
  libunitree_api__rosidl_typesupport_introspection_c.so
  libunitree_api__rosidl_typesupport_fastrtps_c.so
manifest.yaml
checksums.sha256
```

The wrapper RPATH is `$ORIGIN/lib:/opt/ros/<distro>/lib` — bundled
unitree_hg / unitree_api libs are loaded from `lib/`, while ROS 2 system
libraries (`/opt/ros/foxy/lib` or `/opt/ros/humble/lib`) are not bundled.

Runtime notes:

- Defaults `RMW_IMPLEMENTATION` to `rmw_cyclonedds_cpp` if it is unset.
- Overrides `ROS_LOCALHOST_ONLY=1` to `0` because real robot DDS traffic cannot
  be discovered through loopback-only mode.
- If `network_interface` is passed by `SdkContext`, the wrapper sets
  `CYCLONEDDS_URI` to bind CycloneDDS to that interface when the variable is not
  already set.
- Subscribes `/lowstate` and publishes `/lowcmd`.
- `request_control(SDK_CONTROL_READY)` calls the G1 motion switcher
  `/api/motion_switcher/*` API to release active high-level motion services
  before enabling low-level command publication.
