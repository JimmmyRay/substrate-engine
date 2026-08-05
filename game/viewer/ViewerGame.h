#pragma once

#include "Game.h"
#include "core/Input.h"
#include "scene/CameraControllers.h"

/**
 * @file game/viewer/ViewerGame.h
 * @brief The asset viewer, and what `scripts/golden.sh` and `scripts/readback.sh` run when no
 *        game is named.
 *
 * Giving it a world, a scene of its own or a light of its own puts all of that into thirteen
 * golden baselines.
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
