// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/mesh.hpp"
#include <CesiumGltfReader/GltfReader.h>
#include <gz/common/Mesh.hh>
#include <gz/common/MeshManager.hh>
#include <gz/common/Material.hh>
#include <gz/common/SubMesh.hh>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <cstring>
#include <fstream>
#include <set>
#include <unistd.h>

namespace {
class MeshTest : public testing::Test {
 protected:
  void SetUp() override {
    directory = std::filesystem::temp_directory_path()/
        ("gz_cesium_test_"+std::to_string(getpid()));
    std::filesystem::create_directories(directory);
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  CesiumGltf::Model fixture() {
    std::ifstream input(GZ_CESIUM_FIXTURE, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    CesiumGltfReader::GltfReader reader;
    auto result = reader.readGltf(std::span(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (!result.model) throw std::runtime_error("Cannot decode test GLB");
    return std::move(*result.model);
  }
  std::filesystem::path directory;
};

TEST_F(MeshTest, EcefNodeAndUpAxisProduceLocalCoordinatesAndPortableTextures) {
  auto model = fixture();
  model.nodes[0].translation = {0., 2., 0.};  // glTF Y maps to ENU up.
  const auto origin = gz_cesium::enuToEcef(32.3645, -90.8431, 100.);
  const auto tile = origin*glm::translate(glm::dmat4(1.), glm::dvec3(20,-10,3));
  const auto mesh = gz_cesium::prepareMesh(model, tile, glm::inverse(origin), directory, 1);
  ASSERT_FALSE(mesh->vertices.empty());
  EXPECT_NEAR(mesh->vertices[0].x, -12., 1e-7);
  EXPECT_NEAR(mesh->vertices[0].y, -42., 1e-7);
  EXPECT_NEAR(mesh->vertices[0].z, 5., 1e-7);
  ASSERT_TRUE(mesh->heightAt(10., -20.));
  EXPECT_NEAR(*mesh->heightAt(10., -20.), 5., 1e-7);
  EXPECT_FALSE(mesh->heightAt(100., 100.));
  EXPECT_EQ(mesh->textureCount, 1u);
  const auto* loaded = gz::common::MeshManager::Instance()->Load(mesh->uri.string());
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->VertexCount(), mesh->vertices.size());
  EXPECT_GT(loaded->MaterialCount(), 0u);
  ASSERT_NE(loaded->MaterialByIndex(0), nullptr);
  EXPECT_TRUE(std::filesystem::is_regular_file(loaded->MaterialByIndex(0)->TextureImage()));
  EXPECT_EQ(std::filesystem::path(loaded->MaterialByIndex(0)->TextureImage()).parent_path().lexically_normal(),
            mesh->directory/"materials/textures");
  const auto submesh = loaded->SubMeshByIndex(0).lock();
  ASSERT_NE(submesh, nullptr);
  EXPECT_NEAR(submesh->TexCoord(0).Y(), 0., 1e-7);
  gz::common::MeshManager::Instance()->RemoveMesh(mesh->uri.string());
}

TEST_F(MeshTest, TextureBasenamesAreUniqueAcrossTilesAndIndependentSystems) {
  const auto model = fixture();
  const auto origin = gz_cesium::enuToEcef(32.3645, -90.8431, 100.);
  const auto first = gz_cesium::prepareMesh(model, origin, glm::inverse(origin), directory, 17);
  const auto second = gz_cesium::prepareMesh(model, origin, glm::inverse(origin), directory, 18);
  const auto otherSystem = gz_cesium::prepareMesh(model, origin, glm::inverse(origin), directory/"another_system", 17);
  std::set<std::string> resourceNames;
  for (const auto& mesh : {first, second, otherSystem}) {
    const auto* loaded = gz::common::MeshManager::Instance()->Load(mesh->uri.string());
    ASSERT_NE(loaded, nullptr);
    ASSERT_NE(loaded->MaterialByIndex(0), nullptr);
    const auto texture = std::filesystem::path(loaded->MaterialByIndex(0)->TextureImage());
    EXPECT_TRUE(std::filesystem::is_regular_file(texture));
    resourceNames.insert(texture.filename().string());
    gz::common::MeshManager::Instance()->RemoveMesh(mesh->uri.string());
  }
  EXPECT_EQ(resourceNames.size(), 3u);
}

TEST_F(MeshTest, ReflectedNodesKeepTriangleWindingAndContactHeight) {
  auto model = fixture();
  model.nodes[0].scale = {-1., 1., 1.};
  const auto origin = gz_cesium::enuToEcef(32.3645, -90.8431, 100.);
  const auto mesh = gz_cesium::prepareMesh(model, origin, glm::inverse(origin), directory, 2);
  const auto face = mesh->triangles[0];
  const auto normal = glm::cross(mesh->vertices[face[1]]-mesh->vertices[face[0]],
                                mesh->vertices[face[2]]-mesh->vertices[face[0]]);
  EXPECT_GT(normal.z, 0.);
  ASSERT_TRUE(mesh->heightAt(10., 10.));
  EXPECT_NEAR(*mesh->heightAt(10., 10.), 0., 1e-7);
}

TEST_F(MeshTest, CorruptIndicesAreRejectedBeforePhysicsUsesThem) {
  auto model = fixture();
  const auto& primitive = model.meshes[0].primitives[0];
  const auto& accessor = model.accessors[primitive.indices];
  const auto& view = model.bufferViews[accessor.bufferView];
  auto& data = model.buffers[view.buffer].cesium.data;
  std::uint32_t invalid = 0xffffffffu;
  std::memcpy(data.data()+view.byteOffset+accessor.byteOffset, &invalid, sizeof(invalid));
  EXPECT_THROW(gz_cesium::prepareMesh(model, glm::dmat4(1), glm::dmat4(1), directory, 3), std::runtime_error);
}

TEST_F(MeshTest, EmptyHierarchyContentDoesNotFailTerrainLoading) {
  CesiumGltf::Model empty;
  EXPECT_EQ(gz_cesium::prepareMesh(empty, glm::dmat4(1), glm::dmat4(1), directory, 4), nullptr);
}
}  // namespace
