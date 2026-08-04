#pragma once

#include "Game.h"
#include "core/Input.h"
#include "scene/CameraControllers.h"

/**
 * @file game/viewer/ViewerGame.h
 * @brief The asset viewer, and what `scripts/golden.sh` runs.
 *
 * A game that composes nothing. It opens whatever `--scene` named, lights it, gives it a
 * free-fly camera and quits on Escape -- which is the whole of what the golden and readback
 * suites need, and the whole of what somebody looking at one `.glb` needs.
 *
 * **It exists so that no other game has to defend itself against the harness.** The suites
 * run `./run.sh <config>` with no game name, which used to mean "whichever game the build
 * directory happens to hold": the demo's world in thirteen baselines unless the demo checked
 * whether its scene had been overridden, and the arena in them if `battle_arena` was built
 * last. A game does not want its world replaceable from a command line, so the harness gets
 * a binary of its own instead.
 *
 * The lighting is the demo's, value for value, because the baselines were captured through
 * the demo. Changing one of these numbers moves thirteen images.
 */
class ViewerGame : public Game {
  public:
    void configure(GameSetup& setup, core::settings::Settings& settings) override;
    void init(Engine& e) override;
    void frameUpdate(Engine& e, float dt) override;
    void shutdown(Engine& e) override;

  private:
    core::input::ActionId quit{};
    scene::FlyCamera flyCamera;
};
