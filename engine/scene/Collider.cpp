#include "scene/Collider.h"

#include "core/Json.h"
#include "core/Logger.h"

#include <rapidjson/document.h>

#include <cstring>
#include <string>

namespace scene {

namespace {

using rapidjson::Value;
using core::json::gltfJsonSpan;
using core::json::member;
using core::json::readAngleDegrees;
using core::json::readFloat;
using core::json::readString;
using core::json::readVec;

// `gltfJsonSpan` was written here in full when S4.2 landed, with a comment saying the
// duplication against ParticleSystem.cpp was deliberate because two occurrences are a
// coincidence. S5.2's AudioSource.cpp is the third occurrence, so it moved to
// core/Json.h -- which is the rule working as intended rather than a correction.

/// An unrecognised spelling keeps the default *and says so*, which is the one place this
/// schema does not follow `Config`'s "absent keeps the default" rule silently: a typo in
/// a shape name is a collider that is quietly the wrong shape, and the file it came from
/// looks correct.
template <typename Enum, size_t N>
void readEnum(const Value& parent, const char* key, const char* const (&names)[N], Enum& out, const char* what,
              const std::string& owner) {
    std::string text;
    core::json::readString(parent, key, text);
    if (text.empty()) return;
    for (size_t i = 0; i < N; ++i) {
        if (text == names[i]) {
            out = static_cast<Enum>(i);
            return;
        }
    }
    core::Logger::warn(core::LogCategory::Scene, "Collider '%s': unknown %s '%s' -- keeping %s", owner.c_str(), what,
                 text.c_str(), names[static_cast<size_t>(out)]);
}

const char* const kShapeNames[] = {"auto", "box", "sphere", "capsule", "cylinder", "hull", "mesh"};
const char* const kMotionNames[] = {"static", "kinematic", "dynamic", "character"};
const char* const kFreedomNames[] = {"all", "plane2d"};

} // namespace

const char* colliderShapeName(ColliderShape shape) { return kShapeNames[static_cast<size_t>(shape)]; }
const char* colliderMotionName(ColliderMotion motion) { return kMotionNames[static_cast<size_t>(motion)]; }
const char* colliderFreedomName(ColliderFreedom freedom) { return kFreedomNames[static_cast<size_t>(freedom)]; }

bool parseSceneColliders(const rapidjson::Value& nodesArray, std::vector<ColliderDesc>& out) {
    // The document is parsed once, by the caller, and handed to all three readers
    // (C14). It used to be parsed here, and in the other two, and that was about
    // three quarters of a large scene's load -- see core/Json.h.
    const Value* nodes = &nodesArray;

    for (rapidjson::SizeType n = 0; n < nodes->Size(); ++n) {
        const Value* extras = core::json::member((*nodes)[n], "extras");
        if (extras == nullptr) continue;
        const Value* def = core::json::member(*extras, "substrate_collider");
        if (def == nullptr || !def->IsObject()) continue;

        ColliderDesc c;
        c.node = n;
        core::json::readString(*def, "name", c.name);
        if (c.name.empty()) core::json::readString((*nodes)[n], "name", c.name);
        if (c.name.empty()) c.name = "node " + std::to_string(n);

        readEnum(*def, "shape", kShapeNames, c.shape, "shape", c.name);
        readEnum(*def, "motion", kMotionNames, c.motion, "motion", c.name);
        readEnum(*def, "freedom", kFreedomNames, c.freedom, "freedom", c.name);

        core::json::readVec<3>(*def, "halfExtent", &c.halfExtent.x);
        core::json::readFloat(*def, "radius", c.radius);
        core::json::readFloat(*def, "halfHeight", c.halfHeight);
        core::json::readVec<3>(*def, "offset", &c.offset.x);

        readFloat(*def, "mass", c.mass);
        readFloat(*def, "friction", c.friction);
        readFloat(*def, "restitution", c.restitution);
        readFloat(*def, "linearDamping", c.linearDamping);
        readFloat(*def, "angularDamping", c.angularDamping);
        readFloat(*def, "gravityFactor", c.gravityFactor);

        readFloat(*def, "stepHeight", c.stepHeight);
        core::json::readAngleDegrees(*def, "maxSlopeAngle", c.maxSlopeAngle);
        readFloat(*def, "moveSpeed", c.moveSpeed);
        readFloat(*def, "jumpSpeed", c.jumpSpeed);
        readFloat(*def, "acceleration", c.acceleration);
        readFloat(*def, "deceleration", c.deceleration);
        readFloat(*def, "airControl", c.airControl);
        // Steps, and the key says so. Authored as an integer count rather than as seconds
        // because seconds would have to be divided by the fixed step to be used, and a
        // window that rounds is a window whose size depends on the clock it was rounded
        // against -- which is the whole thing these two rows exist to remove.
        core::json::readUint(*def, "jumpBufferSteps", c.jumpBufferSteps);
        core::json::readUint(*def, "coyoteSteps", c.coyoteSteps);

        // A concave triangle mesh has no inertia tensor, so Jolt refuses it a dynamic
        // body outright. Caught here rather than at body creation because this is where
        // the file said it, and the message can name the node.
        if (c.resolvedShape() == ColliderShape::Mesh &&
            (c.motion == ColliderMotion::Dynamic || c.motion == ColliderMotion::Character)) {
            core::Logger::warn(core::LogCategory::Scene,
                         "Collider '%s': a triangle mesh cannot be %s -- using its convex hull instead",
                         c.name.c_str(), colliderMotionName(c.motion));
            c.shape = ColliderShape::Hull;
        }

        out.push_back(std::move(c));
    }
    return true;
}

} // namespace scene
