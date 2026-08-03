// Shared material layout. Must match GpuMaterial in engine/scene/SceneTypes.h exactly.
struct Material {
    vec4 baseColorFactor;
    vec4 emissiveFactor;

    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    float normalScale;

    int baseColorTexture;
    int metallicRoughnessTexture;
    int normalTexture;
    int occlusionTexture;

    int emissiveTexture;
    uint alphaMask;
    // A slot in the *game's* image array (P6), not in the scene's -- set 2 rather than the
    // four int fields above, which index set 1 binding 1. Read only by sprite_lit.frag and
    // sprite_lit_shadow.frag; declared here so the struct matches the buffer byte for byte.
    uint gameImage;
    // Which shader variant draws this material (G5). Declared so the struct matches the
    // buffer byte for byte, and never read here: the CPU groups draw commands by it, so
    // by the time a fragment runs the answer is the pipeline it is running in.
    uint shader;

    // A variant's own four floats (G5). Nothing in engine/shaders/ reads these.
    vec4 params;
};
