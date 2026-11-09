# R4: generic service and client APIs per supported distro

Nov 9, 2026 · research ticket R4 in `docs/00-design.md` (feeds X6 services, issue #156).

**Sources.** The pinned bridge images from `ci/versions.env`
(`ros:jazzy-ros-base-noble@sha256:103b6c41...`,
`ros:lyrical-ros-base-resolute@sha256:d9078270...`,
plus `ros:rolling-ros-base` for the moving snapshot); `rclcpp`
`package.xml` in each image; the installed headers
(`rclcpp`, `rcl`, `rmw`); the matching `ros2/rclcpp`, `ros2/rmw_zenoh`
and `ros2/rmw_cyclonedds` branches; the repo's own e2e setup
(`.github/workflows/ci.yml`, `tests/e2e/run.sh`) and the bridge's
current generic topic path (`bridge/r2s_bridge/src/router.cpp`,
`bridge/r2s_bridge/package.xml`). No repo code was changed for this
inventory; every header claim below was read from the images listed.

**Method.** Each check below runs against the pinned images, for
example `docker run --rm ros:lyrical-ros-base bash -c 'ls
/opt/ros/lyrical/include/rclcpp/rclcpp | grep generic'`. Re-running
those listings reproduces the tables.

## Summary

- `GenericClient` exists on every supported distro: jazzy
  (rclcpp 28.1.22), lyrical (32.0.3), rolling (33.1.0, snapshot).
- `GenericService` exists on lyrical and rolling only. Jazzy has no
  `GenericService` header, no `Node::create_generic_service` and no
  `rclcpp::create_generic_service` free function.
- The design default stands: generic APIs where present,
  introspection-based typed services for the jazzy server side.
  Nothing in this inventory changes what #156 should build, so no
  follow-up ticket is filed.
- The client and service APIs sit on the RMW-agnostic `rcl` C API
  (`rcl_send_request`, `rcl_take_response_with_info`,
  `rcl_take_request_with_info`, `rcl_send_response`, identical on
  jazzy and lyrical). All three RMWs in the e2e matrix
  (`rmw_fastrtps_cpp`, `rmw_cyclonedds_cpp`, `rmw_zenoh_cpp`)
  implement the service entry points on the jazzy and lyrical lines.
  CI installs Cyclone and Zenoh per distro leg and starts the Zenoh
  router first; services need no extra per-RMW work beyond that.

## API inventory with versions

| Distro | rclcpp (measured) | `GenericClient` | `GenericService` |
| --- | --- | --- | --- |
| jazzy | 28.1.22 | yes | no |
| lyrical | 32.0.3 | yes | yes |
| rolling | 33.1.0 (snapshot, moves) | yes | yes |

Node factories follow the same split. Jazzy `Node` offers
`create_generic_client` (plus generic pub/sub) but no
`create_generic_service`; lyrical and rolling `Node` offer both.
The free functions mirror it: `rclcpp/create_generic_client.hpp`
on all three, `rclcpp/create_generic_service.hpp` on lyrical and
rolling only. Every factory defaults QoS to `ServicesQoS` and takes
an optional callback group.

Reference pages: jazzy `GenericClient`, lyrical and rolling
`GenericService`/`GenericClient` under the `generated/` API path of
each distro's docs. Sources: `ros2/rclcpp` branches `jazzy`,
`lyrical` and `rolling` hold `generic_client.hpp` plus
`generic_client.cpp` on all three, and `generic_service.hpp` plus
`generic_service.cpp` on lyrical and rolling.

## RMW inventory

| RMW | jazzy | lyrical | Service entry points |
| --- | --- | --- | --- |
| `rmw_fastrtps_cpp` | in the base image | in the base image | full map |
| `rmw_cyclonedds_cpp` | apt per CI leg, branch implements client/service | apt per CI leg, branch implements client/service | full map |
| `rmw_zenoh_cpp` | apt per CI leg, branch implements client/service | apt per CI leg, branch implements client/service | full map, router required |

Notes: the `-base` images ship only Fast DDS; CI installs
`ros-${DISTRO}-rmw-cyclonedds-cpp` and
`ros-${DISTRO}-rmw-zenoh-cpp` on each distro leg, and `run.sh`
starts `rmw_zenohd` before the Zenoh leg. The Zenoh client and
service creators are real implementations on both the `jazzy` and
`lyrical` branches, not unsupported stubs. CycloneDDS carries the
same entry points on both branches. Bridge CI builds on jazzy and
lyrical; rolling coverage is assigned to the nightly job by the
design but no such job exists yet, so the rolling column needs
re-verification once it lands, not a one-time reading.

## What type-erased request and response handling looks like

Both classes are constructed from plain strings: service name plus
service type as `package/srv/Type`. Construction loads the type's
`rosidl_typesupport_cpp` library at runtime and resolves the
introspection layout (`rosidl_typesupport_introspection_cpp`
`MessageMembers`) for request and response. An uninstalled or
misspelled type throws at construction.

`Request` and `Response` are `void *` to deserialized introspection
structs, not CDR blobs: `create_request` and `create_response`
allocate `size_of_` bytes and run `init_function`, and the matching
`destroy` runs `fini_function`. The client sends with
`async_send_request` into `rcl_send_request`; responses come back
keyed by sequence number into an internal `pending_requests_` map
as `shared_ptr<void>` futures, so a caller that times out must call
`remove_pending_request` or `prune_pending_requests` or the map
grows. The service takes with `take_request` (allocates via
`create_request`, then `take_type_erased_request`), dispatches one
`GenericServiceCallback` slot (plain, with-header, and two
deferred-response shapes), and answers with `send_response` into
`rcl_send_response`.

Jazzy-to-lyrical client deltas are small and caller-compatible: the
request parameter is now a const reference, `handle_response` takes
const references, and lyrical adds `SharedFutureAndRequestId`, a
callback-taking `async_send_request` overload, and
`remove_pending_request` overloads for both future types. The
`Node` and free-function factories changed `SharedPtr` parameters
to const references.

## Gaps a bridge service path has to fill (feeds #156)

1. No generic service server on jazzy. The server side there needs
   the documented fallback (introspection-based typed services);
   only the client side is generic on all three distros.
2. Neither class speaks CDR. Unlike `GenericPublisher`, which moves
   `SerializedMessage` blobs (the bridge's current topic path),
   client and service both deal in deserialized introspection
   structs, so the bridge must convert between the MCU wire encoding
   and those structs field by field through `MessageMembers` in both
   directions, on every distro. The design already names this
   mechanism (`rmw_serialize` after filling the introspection
   struct); the bridge dependency
   (`rosidl_typesupport_introspection_cpp`) is already in place.
3. Runtime type availability: every named type string must resolve
   to an installed `rosidl_typesupport_cpp` library on the bridge
   host, otherwise construction throws. Unknown MCU type names need
   a precise error, never silent wrong bytes.
4. Pending-request bookkeeping is the caller's job: proxying MCU
   calls means mapping MCU request ids to rcl sequence numbers with
   its own timeout and prune path, or entries leak per unanswered
   call.
5. No per-RMW service code paths are needed: the generic APIs call
   only the stable `rcl` layer, and all three RMWs implement the
   service map on the supported branches. The Zenoh leg keeps its
   router requirement, which the e2e harness already handles.

No recommendation and no API proposal here; those stay with the
maintainer.
