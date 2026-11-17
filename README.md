# gz_cesium

Stream 3D Tiles into **Gazebo Jetty / ROS 2 Lyrical** using Cesium Native.
Selected tiles become textured Gazebo mesh entities with **the same triangles
in visuals and collisions**. Ground vehicles and sensors see the streamed
terrain, buildings and trees in one local ENU frame.

This is an early prototype. The fleet integration has been exercised with
`gz_sensors_ouster` in raycast mode, two Warthogs and two Huskies. The adapter
itself does not create lidar sensors; the example worlds contain no built-in
`gpu_lidar` sensors.

## Build

Requires the ROS 2 Lyrical / Gazebo Jetty development packages, a C++20
compiler, CMake, Ninja, Git and colcon. Stop running Cesium scenes before
installing a rebuilt plugin.

```bash
source /opt/ros/lyrical/setup.bash
tools/build_cesium_native 8
colcon build --base-paths . --packages-select gz_cesium \
  --cmake-args -DCMAKE_BUILD_TYPE=Release
source install/setup.bash
ctest --test-dir build/gz_cesium --output-on-failure
```

The helper builds unmodified Apache-2.0 Cesium Native revision
`55d288611b3dd6e1acf865d030f1e9a792be1514` under `.deps/cesium`, including
its pinned private vcpkg dependencies. These dependencies are isolated from
Gazebo's shared libraries. The x64 Linux triplet pins the compiler baseline
explicitly; this avoids implicit AVX defaults breaking the pinned KTX encoder's
SSE-specific translation units on fresh Ubuntu 26.04 runners.
To reuse another build, set
`GZ_CESIUM_NATIVE_ROOT` when running the helper and configuring CMake, or pass
`-DGZ_CESIUM_NATIVE_ROOT=/absolute/dependency/root` to CMake. A package inside
`fleet_sim_ws/src/gz_cesium` also detects the workspace's `runtime/cesium`
dependency build. Source credentials and downloaded tiles are not included.

## Offline example

```bash
gz sim -r worlds/local_tiles.world
```

The authored CC0 fixture needs no token or downloads. It exercises ECEF and
node transforms, glTF up-axis conversion, textures and parent/child LOD.
Regenerate it with `python3 tools/generate_cesium_fixture.py`.

## Caltech photogrammetry

The Caltech example follows HERCULES's verified reference coverage. The ENU
origin is **34.1377301205, -118.1255725710**, at **197.1513594924 m** WGS84
ellipsoid height. It is an open staging patch about 29 m from HERCULES's
reference point, selected by dense source-height sampling. It uses only
Google Photorealistic 3D Tiles through Cesium ion asset **2275207**, with
actual 3D campus buildings and trees. No generated foliage or building
proxies are added.

```bash
export CESIUM_ION_TOKEN_FILE=/absolute/path/to/cesium-token.txt
export GZ_CESIUM_STATUS_FILE="$PWD/runtime/caltech-status.json"
gz sim -r -s --headless-rendering worlds/caltech.world
```

Credentials are read from `CESIUM_ION_TOKEN` or `CESIUM_ION_TOKEN_FILE` at
runtime. A file can contain a bare token or notes with one unique JWT. Do not
put credentials in worlds, command arguments or version control. The probe
can sample the actual source before placing vehicles:

```bash
ros2 run gz_cesium gz_cesium_probe --point 34.1377301205,-118.1255725710
```

The probe reports ellipsoid heights, sample success, geometry counts and
safe source-material metadata. It never prints signed source URLs.
Coverage is location dependent: the selected Vicksburg park patch has terrain
with tree imagery, while Caltech has volumetric buildings and trees.

## RViz meshes and source credits

With the simulator running, use separate terminals with the overlay sourced:

```bash
ros2 run gz_cesium cesium_markers.py --status "$GZ_CESIUM_STATUS_FILE"
rviz2 -d "$(ros2 pkg prefix gz_cesium)/share/gz_cesium/config/cesium.rviz"
```

The publisher mirrors active mesh resources onto `/cesium/tiles`, removes
retired markers, and preserves source credits as visible text and on
`/cesium/credits`. Use this view to display the native source attributions;
a Gazebo GUI attribution overlay is not implemented yet. Local mode uses
temporary file URIs. For a viewer on another machine, run the asset service
beside Gazebo and the marker publisher beside RViz:

```bash
# Simulator host; optionally include --session /path/to/prepared/fleet
# to export rewritten robot descriptions and their optimized meshes as well.
ros2 run gz_cesium serve_assets.py --status "$GZ_CESIUM_STATUS_FILE" --bind 0.0.0.0 --port 8899
# Viewer host, with a reachable DDS configuration/domain for live robots:
ros2 run gz_cesium cesium_markers.py --remote http://SIMULATOR:8899 \
  --cache-dir "$HOME/.cache/gz_cesium"
```

The service exports only referenced assets, with content hashes, gzip and ETags;
it does not export source credentials or proxy provider requests. The viewer
verifies downloads, retains its last good scene during connection errors, and
reuses unchanged files after reconnecting. The server/client cache targets are
64/256 MiB; active resources are retained even if they exceed those targets.
The default viewer textures are capped at 512 pixels with opaque atlases encoded
as JPEG quality 90; use `serve_assets.py --texture-size 0` for original textures.
This affects viewer copies only. Geometry and simulator source assets are
preserved. Bind to a trusted interface or use an SSH tunnel; this small asset
service provides no authentication. Live ROS topics still use DDS separately.

The `--watch-pid PID` option exits the publisher when an owning viewer exits.

## Plugin configuration

See `worlds/local_tiles.world` and `worlds/caltech.world` for examples.

| Setting | Meaning |
| --- | --- |
| `latitude`, `longitude`, `height` | WGS84 ENU origin: degrees and ellipsoid metres. Match the world's `spherical_coordinates`. |
| `ion_asset_id` | Cesium ion asset, default 2275207. Up to four repeated assets are supported. |
| `tileset_url` | Direct 3D Tiles URL or local file, instead of ion. `GZ_CESIUM_TILESET_URL` overrides it. |
| `follow_model` | Repeated dynamic model names. By default, track every non-static model. |
| `stream_radius` | Native selection radius around vehicles, default 150 m. Bounding volumes may straddle the boundary. |
| `contact_radius` | Retain settled local contact tiles near vehicles, default 15 m. |
| `maximum_screen_space_error` | Native LOD threshold: default 4, Caltech 2. |
| `maximum_simultaneous_tile_loads` | Native loading limit, default 8, shared across configured ion layers. |
| `maximum_cached_bytes` | Native cache target, default 256 MiB, shared across layers. |
| `update_interval` | Wall-clock selection period, default 0.2 seconds, including while paused. |
| `wait_for_terrain` | Pause startup until settled mesh coverage exists under every vehicle footprint. |
| `place_on_ground` | Initially add terrain height to each model's Z. Disable for pre-sampled absolute spawn poses. |

Each vehicle contributes six surrounding views and a downward contact view.
Local collision tiles are pinned after selection settles; their matching
visuals remain pinned too, avoiding overlapping parent/child contact surfaces.
Pins expire as vehicles move away. Conversion runs in worker threads; entity
creation/removal happens on Gazebo's update thread. Retired CPU meshes and
temporary DAE/PNG files are released after a grace period; the private tile
directory is removed on normal shutdown. Texture basenames are unique across
tiles and systems because Ogre caches images by basename.

JSON status is published on `/world/<world>/cesium/status` and atomically
written to `GZ_CESIUM_STATUS_FILE` (default: `status.json` inside the private
tile directory). It includes readiness, load progress, active/pinned tiles,
local geometry paths/counts, credits and preparation errors. Native diagnostics
redact authentication query values and JWTs.

## Validation and limits

Nine C++ tests cover ENU/node/up-axis transforms, portable textures,
resource-name isolation, reflected winding, malformed indices, empty tile
hierarchies, unlit material export, transient GET recovery, permanent failures
and bounded retry shutdown. Seven HTTP tests cover separate caches,
warm reuse, corrupt-transfer retries, reconnection, resource restrictions,
eviction and texture conversion. `tools/ci` builds the pinned dependency and
configures, builds, installs and tests a fresh package build; the GitHub Actions
workflow uses Ubuntu 26.04, ROS Lyrical and Gazebo Jetty.

The Caltech fleet passed driving, braking, terrain contact,
cameras, Ouster metadata/packets/clouds and 3D TF checks with about 416,000
active triangles. A separate authored 512 m course exercised all five fleet
platforms over 318 m each in 350 simulated seconds (354 wall seconds):
74 tile creations, 60 removals, 80 collected native resources and no preparation
failures. A 12-second provider outage produced 12 successful retries; a separate
viewer outage recovered while retaining its last good scene. The viewer used
6.95 MB of HTTP data, 6,707 cache hits and 120 evictions. Largest-process RSS
rose from 1,529 to 1,658 MiB and settled near that peak. This finite traversal
does not establish a hard memory bound or hours-long mission reliability.

The adapter handles triangle glTF content and base-color textures. Materials
marked `KHR_materials_unlit` use emissive texture maps with zero lit diffuse,
preserving the source photogrammetry without darkening it under Gazebo lighting.
Ordinary materials keep PBR lighting. Each primitive retains its material and
the combined collision mesh uses the same triangles.
Raster overlays, Gaussian splats, animation and advanced material extensions
are not implemented. Photogrammetry can have coarse or missing surfaces under
foliage and around narrow passages; inspect the source before placing robots.

Transient GET transport errors, HTTP 408/429 and 5xx responses retry up to
eight attempts, using backoff from 0.5 seconds capped at eight seconds. Other
HTTP responses and mutation requests are not replayed. Queued retries are
cancelled on shutdown. Longer outages can exhaust this budget; retained contact
geometry does not provide coverage in unloaded regions. Status includes
request/retry/failure counts, native cache bytes and retired/collected resources.

The native cache target does not bound total RSS: pinned contact geometry,
exported images, physics, sensor mirrors and renderer caches are additional.
Hours-long traversals and total renderer-cache growth still need testing. Source
imagery retains its provider terms and is not covered by this package's
Apache-2.0 license; only the authored fixture is CC0.
