#include "scene/Collider.h"
#include "scene/Physics.h"
#include "scene/SceneTypes.h"
#include "scene/SpriteTable.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace scene;

namespace fs = std::filesystem;

/**
 * @file tests/TilemapTests.cpp
 * @brief P8's outcome: there is no tilemap, and this is what a game writes instead.
 *
 * The row was reconsidered at the Phase 3 boundary, as `docs/kanban/order.md` required,
 * and declined. `docs/architecture/limitations.md` carries the argument and the two
 * triggers that would reverse it. **This file is the part of that decision that could
 * otherwise rot**: a refusal whose replacement is a code sketch in a document is a refusal
 * nobody can check, so the sketch lives here, compiles against the real headers, and runs
 * in the suite under every sanitizer.
 *
 * Nothing below is engine code. Every line is what a *game* writes, over four things the
 * engine already has:
 *
 * | The tilemap wants | What answers it | Row |
 * |---|---|---|
 * | A cell's rectangle in the tileset | `SpriteTable::frameUv(sheet, cell)` | P5 |
 * | Drawing the grid | `SpriteTable::create` into one layer -- one draw for all of it | P4 |
 * | Scrolling it | `setPosition` / `setUv`, which re-sort nothing | P4 |
 * | Collision from the grid | Merged runs, `ColliderFreedom::Plane2D` | P7 |
 * | A chunk as lit geometry | A `MeshData`, handed to `Engine::createMesh` | G4 |
 *
 * The one call this file cannot make is the last: `createMesh` takes a `VulkanContext` and
 * an `Uploader`, so the test builds the `MeshData` -- which is all the arithmetic -- and
 * stops one line short of the device. `LitSpriteTests.cpp` covers the other side.
 *
 * ## The map is in the game's format, and that is the whole point
 *
 * Sixteen characters a row, because that is legible in a test. A real game would use its
 * own array, its own editor's export, or a procedural generator. The engine reads none of
 * them and invents none of them, which is the half of P8 that was declined hardest: a
 * tilemap format the engine authored would be a format with one tool and no asset.
 */

namespace {

/// Texels per cell, and world units per cell -- one texel per unit is what makes a sprite
/// pixel-exact (P2/P4), so a 2D game usually picks the same number for both.
constexpr float kTile = 16.0f;
constexpr uint32_t kWidth = 16;
constexpr uint32_t kHeight = 8;

/// '#' is ground and collides, '~' is decoration and does not, '.' is nothing at all.
constexpr std::array<std::string_view, kHeight> kMap = {
    "................", //
    "................", //
    "......###.......", //
    "..~~~...........", //
    "...##...........", //
    "................", //
    "...........~~~..", //
    "################", //
};

constexpr uint32_t kSolidCells = 21;
constexpr uint32_t kFilledCells = 27;

/// Which cell of the tileset a map character draws. The game's table, and the engine has
/// no opinion about it.
constexpr uint32_t kGroundCell = 1;
constexpr uint32_t kDecorCell = 5;

[[nodiscard]] char cellAt(uint32_t x, uint32_t y) { return kMap[y][x]; }
[[nodiscard]] bool filled(char c) { return c != '.'; }
[[nodiscard]] bool solid(char c) { return c == '#'; }
[[nodiscard]] uint32_t tileCell(char c) { return c == '#' ? kGroundCell : kDecorCell; }

/**
 * Where cell (x, y) goes in the world. Row 0 is the top of the map, so y runs downward
 * while world +Y runs up -- the one conversion a tilemap has, and it is one negation.
 *
 * The sprite's pivot is its top-left, so the cell occupies `[x, x+1) x (y, y-1]` tiles.
 */
[[nodiscard]] glm::vec2 cellOrigin(uint32_t x, uint32_t y) {
    return {static_cast<float>(x) * kTile, -static_cast<float>(y) * kTile};
}

[[nodiscard]] glm::vec3 positionOf(const PhysicsWorld& world, BodyId body) {
    return glm::vec3(world.bodyTransform(body, 0.0f)[3]);
}

/**
 * @brief Horizontal runs of solid cells, merged into one static box each.
 *
 * Greedy in one axis, which is the cheap two thirds of the classic answer and enough for
 * every map whose ground is rows. Twenty-one solid cells become three bodies here. A game
 * that wants the other axis too writes the second loop; a game that wants neither passes
 * one box per cell and pays for it, which is also a decision it is allowed to make.
 *
 * `Plane2D` is what keeps the whole thing two-dimensional without a second solver.
 */
[[nodiscard]] std::vector<ColliderDesc> mergeSolidRuns() {
    std::vector<ColliderDesc> out;
    for (uint32_t y = 0; y < kHeight; ++y) {
        uint32_t x = 0;
        while (x < kWidth) {
            if (!solid(cellAt(x, y))) {
                ++x;
                continue;
            }
            uint32_t run = 0;
            while (x + run < kWidth && solid(cellAt(x + run, y))) ++run;

            const glm::vec2 origin = cellOrigin(x, y);
            const float halfWidth = static_cast<float>(run) * kTile * 0.5f;

            ColliderDesc c;
            c.name = "ground";
            c.shape = ColliderShape::Box;
            c.motion = ColliderMotion::Static;
            c.halfExtent = {halfWidth, kTile * 0.5f, kTile * 0.5f};
            c.transform = glm::translate(glm::mat4(1.0f),
                                         glm::vec3(origin.x + halfWidth, origin.y - kTile * 0.5f, 0.0f));
            out.push_back(std::move(c));
            x += run;
        }
    }
    return out;
}

/**
 * @brief One chunk of the map as a single `MeshData`, ready for `Engine::createMesh`.
 *
 * The path a game takes when its tiles must be *lit* rather than drawn flat after the
 * tonemap -- P6's trade, applied to a chunk instead of one card. It is a different mesh
 * from `scene::quadMesh` for one reason worth stating: that helper puts the texel rect on
 * the *material* and gives the quad 0..1 corners, which is right when every quad has its
 * own material and wrong for a chunk, where one mesh is one material over a whole atlas.
 * So the UVs here are normalised against the atlas, and the game divides -- it authored
 * the tileset, so it is the one party that knows its size.
 */
[[nodiscard]] MeshData chunkMesh(const SpriteTable& sprites, SpriteSheetId sheet, uint32_t chunkX,
                                 uint32_t chunkY, uint32_t span, const glm::vec2& atlas) {
    MeshData mesh;
    for (uint32_t y = chunkY; y < chunkY + span && y < kHeight; ++y) {
        for (uint32_t x = chunkX; x < chunkX + span && x < kWidth; ++x) {
            const char c = cellAt(x, y);
            if (!filled(c)) continue;

            const glm::vec2 at = cellOrigin(x, y);
            const glm::vec4 uv = sprites.frameUv(sheet, tileCell(c));
            const glm::vec2 uv0 = glm::vec2(uv.x, uv.y) / atlas;
            const glm::vec2 uv1 = glm::vec2(uv.x + uv.z, uv.y + uv.w) / atlas;

            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            const glm::vec3 normal{0.0f, 0.0f, 1.0f};
            const glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};

            mesh.vertices.push_back({{at.x, at.y, 0.0f}, normal, tangent, {uv0.x, uv0.y}});
            mesh.vertices.push_back({{at.x + kTile, at.y, 0.0f}, normal, tangent, {uv1.x, uv0.y}});
            mesh.vertices.push_back({{at.x + kTile, at.y - kTile, 0.0f}, normal, tangent, {uv1.x, uv1.y}});
            mesh.vertices.push_back({{at.x, at.y - kTile, 0.0f}, normal, tangent, {uv0.x, uv1.y}});

            for (const uint32_t offset : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + offset);
        }
    }
    // Cutout, for the same reason P6's lit sprite is: pixel art has holes, and a masked
    // draw keeps depth, velocity and TAA motion correction where a blend gives them up.
    mesh.masked = true;
    return mesh;
}

} // namespace

class TilemapTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const auto unique = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        root = fs::temp_directory_path() / ("substrate_tilemap_" + std::string(unique));
        fs::remove_all(root);
        fs::create_directories(root);
        images.init(16);
        sprites.init(&images);

        const fs::path path = root / "tileset.png";
        std::ofstream(path) << "not an image; ImageTable only asks that the file exists";
        tileset = images.load(path.string());

        // 128x64 texels of 16x16 cells: eight across, four down. The four numbers an
        // artist reads off the tool that drew the sheet, which is the whole of "slicing"
        // and the reason no tileset *file format* was ever needed.
        sheet = sprites.createSheet({.frame = {16, 16}, .columns = 8, .count = 32});
        layer = sprites.createLayer({.order = -10});
    }

    void TearDown() override {
        sprites.shutdown();
        images.shutdown();
        fs::remove_all(root);
    }

    /// **The tilemap, in full.** One pass over the game's grid, one `create` per filled
    /// cell, and the cell's rectangle comes from `frameUv` with no playback attached --
    /// which is exactly what that method's header says it is public for.
    std::vector<SpriteId> buildMap() {
        std::vector<SpriteId> tiles;
        tiles.reserve(kFilledCells);
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                const char c = cellAt(x, y);
                if (!filled(c)) continue;
                tiles.push_back(sprites.create(layer, {
                                                          .image = tileset,
                                                          .uv = sprites.frameUv(sheet, tileCell(c)),
                                                          .size = {kTile, kTile},
                                                          .pivot = {0.0f, 0.0f},
                                                          .position = cellOrigin(x, y),
                                                      }));
            }
        }
        return tiles;
    }

    fs::path root;
    gfx::ImageTable images;
    SpriteTable sprites;
    gfx::ImageId tileset;
    SpriteSheetId sheet;
    SpriteLayerId layer;
};

// --------------------------------------------------------------------------- drawing

TEST_F(TilemapTest, AGridBecomesSpritesThroughFrameUv) {
    const std::vector<SpriteId> tiles = buildMap();

    ASSERT_EQ(tiles.size(), kFilledCells);
    EXPECT_EQ(sprites.count(), kFilledCells);
    for (const SpriteId id : tiles) EXPECT_TRUE(sprites.valid(id));

    sprites.prepare();
    ASSERT_EQ(sprites.draws().size(), kFilledCells);

    // Inside one layer the tiebreak is creation order, so the draw array is the map read
    // row-major. That is a total order and therefore reproducible run to run, which is the
    // property a tilemap needs most and the one P4 already guarantees.
    const GpuSprite& first = sprites.draws()[0];
    EXPECT_EQ(first.posSize.x, cellOrigin(6, 2).x);
    EXPECT_EQ(first.posSize.y, cellOrigin(6, 2).y);
    EXPECT_EQ(first.posSize.z, kTile);
    EXPECT_EQ(first.posSize.w, kTile);
    EXPECT_EQ(first.uvRect, sprites.frameUv(sheet, kGroundCell));

    // Cell 1 of an eight-column sheet of 16x16 cells is the second across on the top row.
    EXPECT_EQ(sprites.frameUv(sheet, kGroundCell), glm::vec4(16.0f, 0.0f, 16.0f, 16.0f));
    // Cell 5 is still the top row; cell 8 would be the second.
    EXPECT_EQ(sprites.frameUv(sheet, kDecorCell), glm::vec4(80.0f, 0.0f, 16.0f, 16.0f));
}

TEST_F(TilemapTest, TheWholeMapIsOneImageAndThereforeOneDraw) {
    buildMap();
    sprites.prepare();

    // The measurement that decided the row. Every tile resolves to the same descriptor
    // slot, and `Renderer::recordSprites` issues one `vkCmdDraw` over the whole array --
    // so the alternative to chunking is not one draw per tile, it is one draw. P4 measured
    // ten thousand of these at 0.053 ms, and a screen of 16 px tiles at 1080p is 8,160.
    const uint32_t slot = sprites.draws()[0].meta.x;
    EXPECT_NE(slot, gfx::ImageTable::kFallbackSlot);
    for (const GpuSprite& s : sprites.draws()) EXPECT_EQ(s.meta.x, slot);
}

TEST_F(TilemapTest, ScrollingTheMapReSortsNothing) {
    const std::vector<SpriteId> tiles = buildMap();
    sprites.prepare();
    const uint64_t sortsAfterBuild = sprites.sortCount();
    ASSERT_GT(sortsAfterBuild, 0u);

    // A scroll of one column: every tile takes the rectangle of the cell to its right and
    // slides one tile left. This is the recycling loop a game writes instead of a chunked
    // tilemap, and it is the reason the cost argument for one collapsed -- a layer is the
    // sort key and a position is not part of it, so rewriting all 27 sprites every frame
    // sorts nothing at all.
    for (uint32_t step = 0; step < 4; ++step) {
        size_t i = 0;
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                if (!filled(cellAt(x, y))) continue;
                const uint32_t source = (x + step + 1) % kWidth;
                sprites.setUv(tiles[i], sprites.frameUv(sheet, tileCell(cellAt(source, y))));
                sprites.setPosition(tiles[i], cellOrigin(x, y) - glm::vec2(kTile * static_cast<float>(step), 0.0f));
                ++i;
            }
        }
        sprites.prepare();
    }

    EXPECT_EQ(sprites.sortCount(), sortsAfterBuild) << "scrolling the map re-sorted it";
    EXPECT_EQ(sprites.count(), kFilledCells);
}

TEST_F(TilemapTest, DestroyingTheLayerTakesTheWholeMapWithIt) {
    const std::vector<SpriteId> tiles = buildMap();
    sprites.prepare();

    // Unloading a level is one call, because a layer already owns the lifetime of what is
    // in it. A tilemap subsystem would have had to invent this and would have invented it
    // the same way.
    sprites.destroyLayer(layer);
    EXPECT_EQ(sprites.count(), 0u);
    for (const SpriteId id : tiles) EXPECT_FALSE(sprites.valid(id));

    sprites.prepare();
    EXPECT_TRUE(sprites.draws().empty());
}

// ------------------------------------------------------------------------ collision

TEST_F(TilemapTest, SolidRunsMergeIntoFarFewerBodies) {
    const std::vector<ColliderDesc> ground = mergeSolidRuns();

    // Three runs -- a ledge, a step and the floor -- out of twenty-one solid cells.
    EXPECT_EQ(ground.size(), 3u);
    EXPECT_LT(ground.size(), kSolidCells);
    for (const ColliderDesc& c : ground) {
        EXPECT_EQ(c.shape, ColliderShape::Box);
        EXPECT_EQ(c.motion, ColliderMotion::Static);
    }

    // The floor is the last run found and spans the full width.
    EXPECT_FLOAT_EQ(ground.back().halfExtent.x, static_cast<float>(kWidth) * kTile * 0.5f);
}

TEST_F(TilemapTest, APlaneLockedBodyLandsOnTheMergedGround) {
    PhysicsWorld world;

    // Gravity in the game's units. At one texel per world unit a 16 px tile is 16 units,
    // so 9.81 m/s^2 is 9.81 texels and everything falls like a feather. Scaling it here is
    // the whole of "the tilemap has a unit scale", and it is a config field rather than
    // anything the engine needs to know about grids.
    PhysicsConfig cfg;
    cfg.gravity = {0.0f, -9.81f * kTile, 0.0f};
    world.init(cfg, 16);

    for (const ColliderDesc& c : mergeSolidRuns()) world.createBody(c);

    ColliderDesc crate;
    crate.name = "crate";
    crate.shape = ColliderShape::Box;
    crate.motion = ColliderMotion::Dynamic;
    crate.freedom = ColliderFreedom::Plane2D;
    crate.halfExtent = glm::vec3(kTile * 0.25f);
    crate.transform = glm::translate(glm::mat4(1.0f), glm::vec3(kTile * 8.0f, -kTile * 3.0f, 0.0f));
    const BodyId body = world.createBody(crate);
    world.finalize();
    ASSERT_TRUE(body.valid());

    for (int i = 0; i < 240; ++i) {
        world.step(1.0f / 60.0f);
        // Exactly, not nearly: P7's constraint is the solver's inverse mass rather than a
        // correction applied afterwards, so a tilemap game's third axis never exists.
        ASSERT_EQ(positionOf(world, body).z, 0.0f) << "left the plane on step " << i;
    }

    // The floor is map row 7, whose top surface is at world y = -7 tiles. A crate of half
    // extent kTile/4 comes to rest a quarter tile above it.
    const float floorTop = cellOrigin(0, 7).y;
    EXPECT_NEAR(positionOf(world, body).y, floorTop + kTile * 0.25f, 0.5f);

    world.shutdown();
}

// ----------------------------------------------------------------- chunks, when lit

TEST_F(TilemapTest, AChunkOfTilesIsOneMeshData) {
    // Eight by eight, which is the chunk a game picks when it wants its tiles lit and
    // therefore needs geometry rather than sprites. Fifteen filled cells fall inside it.
    const MeshData mesh = chunkMesh(sprites, sheet, 0, 0, 8, {128.0f, 64.0f});

    constexpr size_t kChunkTiles = 15;
    EXPECT_EQ(mesh.vertices.size(), kChunkTiles * 4);
    EXPECT_EQ(mesh.indices.size(), kChunkTiles * 6);
    EXPECT_TRUE(mesh.masked);

    // Every index is in range -- the one thing a hand-rolled chunk mesh gets wrong, and
    // the reason the loop rebases off `vertices.size()` rather than a running counter.
    for (const uint32_t index : mesh.indices) EXPECT_LT(index, mesh.vertices.size());

    // The first filled cell of the chunk is (6, 2), and its top-left vertex carries cell 1
    // of the tileset normalised against the atlas: texel 16 of 128 across, 0 of 64 down.
    EXPECT_EQ(mesh.vertices[0].position, glm::vec3(cellOrigin(6, 2), 0.0f));
    EXPECT_FLOAT_EQ(mesh.vertices[0].uv.x, 16.0f / 128.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[0].uv.y, 0.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[2].uv.x, 32.0f / 128.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[2].uv.y, 16.0f / 64.0f);

    // What follows in a game is one line the suite cannot reach:
    //     const auto chunk = e.createMesh(std::move(mesh));
    // and `e.removeModel(chunk)` unloads it. Nothing else is needed, and nothing
    // in it is tilemap-shaped -- which is the finding that closed P8.
    EXPECT_EQ(mesh.material, 0u);
    EXPECT_EQ(mesh.transform, glm::mat4(1.0f));
}
