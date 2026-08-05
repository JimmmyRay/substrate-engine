#include "scene/ParticleEmitter.h"

#include "core/Json.h"

#include <rapidjson/document.h>

#include <string>

namespace scene {

namespace {

using rapidjson::Value;
using core::json::readFloat;
using core::json::readVec;

} // namespace

bool parseSceneEmitters(const rapidjson::Value& nodesArray, std::vector<ParticleEmitter>& out) {
    const Value* nodes = &nodesArray;

    for (rapidjson::SizeType n = 0; n < nodes->Size(); ++n) {
        const Value* extras = core::json::member((*nodes)[n], "extras");
        if (extras == nullptr) continue;
        const Value* def = core::json::member(*extras, "substrate_emitter");
        if (def == nullptr || !def->IsObject()) continue;

        ParticleEmitter e;
        e.node = n;
        core::json::readString(*def, "name", e.name);
        if (e.name.empty()) core::json::readString((*nodes)[n], "name", e.name);
        // Every diagnostic naming an emitter uses this, so an unnamed node must still
        // come out identifiable -- matching what a collider and an audio source report.
        if (e.name.empty()) e.name = "node " + std::to_string(n);

        core::json::readFloat(*def, "rate", e.rate);
        core::json::readFloat(*def, "lifetime", e.lifetime);
        readFloat(*def, "lifetimeJitter", e.lifetimeJitter);

        core::json::readVec<3>(*def, "velocity", &e.velocity.x);
        readFloat(*def, "speedJitter", e.speedJitter);
        core::json::readAngleDegrees(*def, "coneAngle", e.coneAngle);
        core::json::readVec<3>(*def, "boxExtent", &e.boxExtent.x);

        readVec<3>(*def, "gravity", &e.gravity.x);
        readFloat(*def, "drag", e.drag);

        readVec<4>(*def, "colorStart", &e.colorStart.x);
        readVec<4>(*def, "colorEnd", &e.colorEnd.x);
        readFloat(*def, "sizeStart", e.sizeStart);
        readFloat(*def, "sizeEnd", e.sizeEnd);

        core::json::readUint(*def, "texture", e.texture);
        core::json::readUint(*def, "flipbookCols", e.flipbookCols);
        core::json::readUint(*def, "flipbookRows", e.flipbookRows);
        readFloat(*def, "flipbookLoops", e.flipbookLoops);
        readFloat(*def, "spin", e.spin);
        readFloat(*def, "erosion", e.erosion);
        core::json::readBool(*def, "emissive", e.emissive);
        readFloat(*def, "emissiveIntensity", e.emissiveIntensity);
        core::json::readBool(*def, "collides", e.collides);
        readFloat(*def, "restitution", e.restitution);

        out.push_back(std::move(e));
    }
    return true;
}

} // namespace scene
