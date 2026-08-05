#include "gfx/SpirvReflect.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace gfx {
namespace {

// Names and numeric values are the SPIR-V specification's, so they can be looked up
// against its tables rather than reasoned about here.
constexpr uint32_t kMagic = 0x07230203u;
constexpr size_t kHeaderWords = 5;

enum Op : uint32_t {
    OpName = 5,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpVariable = 59,
    OpDecorate = 71,
};

enum Decoration : uint32_t {
    DecorationBlock = 2,
    DecorationBufferBlock = 3,
    DecorationBuiltIn = 11,
    DecorationBinding = 33,
    DecorationDescriptorSet = 34,
    DecorationSpecId = 1,
};

enum StorageClass : uint32_t {
    StorageClassUniformConstant = 0,
    StorageClassUniform = 2,
    StorageClassStorageBuffer = 12,
};

/// What a variable's pointee type turns out to be, once arrays are peeled off.
enum class Pointee { Unknown, Image, StorageImage, Sampler, SampledImage, Struct };

} // namespace

ReflectedModule reflectSpirv(const std::vector<uint32_t>& code) {
    ReflectedModule out;
    if (code.size() < kHeaderWords || code[0] != kMagic) return out;

    std::unordered_map<uint32_t, uint32_t> decorationSet;     // id -> descriptor set
    std::unordered_map<uint32_t, uint32_t> decorationBinding; // id -> binding
    std::unordered_set<uint32_t> isBuiltIn;
    std::unordered_set<uint32_t> blockStructs;       // OpTypeStruct ids decorated Block
    std::unordered_set<uint32_t> bufferBlockStructs; // ... decorated BufferBlock
    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, uint32_t> constants; // id -> literal value

    // Type graph, only the edges needed to answer "what kind of descriptor is this".
    std::unordered_map<uint32_t, Pointee> typeKind;
    std::unordered_map<uint32_t, uint32_t> arrayElement; // array id -> element type
    std::unordered_map<uint32_t, uint32_t> arrayLength;  // array id -> count, 0 unsized
    struct PointerInfo {
        uint32_t storageClass = 0;
        uint32_t pointee = 0;
    };
    std::unordered_map<uint32_t, PointerInfo> pointers;

    struct VariableInfo {
        uint32_t id = 0;
        uint32_t pointerType = 0;
        uint32_t storageClass = 0;
    };
    std::vector<VariableInfo> variables;

    // One forward pass: SPIR-V requires types and decorations to precede the OpVariables
    // using them. The variables are still resolved after the loop, because OpName may
    // follow anything.
    size_t i = kHeaderWords;
    while (i < code.size()) {
        const uint32_t word = code[i];
        const uint32_t opcode = word & 0xFFFFu;
        const uint32_t length = word >> 16;

        // A zero-length instruction loops forever and a length past the end reads out of
        // bounds; both are reachable from a truncated module.
        if (length == 0 || i + length > code.size()) return {};

        switch (opcode) {
        case OpName:
            if (length >= 3) {
                const char* chars = reinterpret_cast<const char*>(&code[i + 2]);
                names[code[i + 1]] = std::string(chars);
            }
            break;

        case OpDecorate:
            if (length >= 3) {
                const uint32_t target = code[i + 1];
                switch (code[i + 2]) {
                case DecorationDescriptorSet:
                    if (length >= 4) decorationSet[target] = code[i + 3];
                    break;
                case DecorationBinding:
                    if (length >= 4) decorationBinding[target] = code[i + 3];
                    break;
                case DecorationSpecId:
                    if (length >= 4) out.specConstantIds.push_back(code[i + 3]);
                    break;
                case DecorationBuiltIn: isBuiltIn.insert(target); break;
                case DecorationBlock: blockStructs.insert(target); break;
                case DecorationBufferBlock: bufferBlockStructs.insert(target); break;
                default: break;
                }
            }
            break;

        case OpTypeImage:
            // Word 7 is `Sampled`: 1 sampled, 2 storage, 0 either. glslang always
            // emits 1 or 2 for Vulkan.
            if (length >= 8) typeKind[code[i + 1]] = code[i + 7] == 2 ? Pointee::StorageImage : Pointee::Image;
            break;
        case OpTypeSampler: typeKind[code[i + 1]] = Pointee::Sampler; break;
        case OpTypeSampledImage: typeKind[code[i + 1]] = Pointee::SampledImage; break;
        case OpTypeStruct: typeKind[code[i + 1]] = Pointee::Struct; break;

        case OpTypeArray:
            if (length >= 4) {
                arrayElement[code[i + 1]] = code[i + 2];
                // The length is an OpConstant id, which SPIR-V guarantees has already
                // been seen -- a second pass would be needed otherwise.
                const auto it = constants.find(code[i + 3]);
                arrayLength[code[i + 1]] = it != constants.end() ? it->second : 0u;
            }
            break;
        case OpTypeRuntimeArray:
            if (length >= 3) {
                arrayElement[code[i + 1]] = code[i + 2];
                arrayLength[code[i + 1]] = 0; // unsized: nothing to check the count against
            }
            break;

        case OpTypePointer:
            if (length >= 4) pointers[code[i + 1]] = {code[i + 2], code[i + 3]};
            break;

        case OpConstant:
            // Only single-word literals, which is all an array length ever is.
            if (length == 4) constants[code[i + 2]] = code[i + 3];
            break;

        case OpVariable:
            if (length >= 4) variables.push_back({code[i + 2], code[i + 1], code[i + 3]});
            break;

        default: break;
        }

        i += length;
    }

    for (const VariableInfo& v : variables) {
        if (isBuiltIn.count(v.id) != 0) continue;

        const auto setIt = decorationSet.find(v.id);
        const auto bindingIt = decorationBinding.find(v.id);
        // No set and binding means it is not a descriptor: an input, an output, a
        // push-constant block or a private variable.
        if (setIt == decorationSet.end() || bindingIt == decorationBinding.end()) continue;

        const auto ptrIt = pointers.find(v.pointerType);
        if (ptrIt == pointers.end()) continue;

        // Peel array wrappers: `sampler2D textures[]` binds as one descriptor with many
        // elements, not as many descriptors.
        uint32_t pointee = ptrIt->second.pointee;
        uint32_t count = 1;
        while (true) {
            const auto arrayIt = arrayElement.find(pointee);
            if (arrayIt == arrayElement.end()) break;
            count = arrayLength[pointee];
            pointee = arrayIt->second;
        }

        const auto kindIt = typeKind.find(pointee);
        const Pointee kind = kindIt != typeKind.end() ? kindIt->second : Pointee::Unknown;

        ReflectedBinding b;
        b.set = setIt->second;
        b.binding = bindingIt->second;
        b.count = count;
        const auto nameIt = names.find(v.id);
        if (nameIt != names.end()) b.name = nameIt->second;

        switch (kind) {
        case Pointee::SampledImage: b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
        case Pointee::StorageImage: b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
        case Pointee::Image: b.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; break;
        case Pointee::Sampler: b.type = VK_DESCRIPTOR_TYPE_SAMPLER; break;
        case Pointee::Struct:
            // Which kind of buffer depends on how the struct is decorated, and on the
            // storage class: SPIR-V 1.3 moved SSBOs from Uniform+BufferBlock to
            // StorageBuffer+Block, and glslang emits the newer form for Vulkan 1.3.
            if (v.storageClass == StorageClassStorageBuffer || bufferBlockStructs.count(pointee) != 0) {
                b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            } else if (v.storageClass == StorageClassUniform && blockStructs.count(pointee) != 0) {
                b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            }
            break;
        case Pointee::Unknown:
            // Left as MAX_ENUM, which the caller reads as "no opinion" and skips.
            break;
        }

        if (v.storageClass != StorageClassUniformConstant && kind != Pointee::Struct) continue;
        out.bindings.push_back(b);
    }

    std::sort(out.specConstantIds.begin(), out.specConstantIds.end());
    out.specConstantIds.erase(std::unique(out.specConstantIds.begin(), out.specConstantIds.end()),
                              out.specConstantIds.end());
    return out;
}

} // namespace gfx
