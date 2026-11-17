// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gz_cesium/mesh.hpp"
#include <sdf/Model.hh>

namespace gz_cesium {
// One collision mesh and non-overlapping visual primitives share ENU vertices.
sdf::Model tileModel(const TileMesh& mesh);
}
