#pragma once
// Rewrites the recompiled shaders' runtime descriptor arrays into fixed-size
// arrays.
//
// XenosRecomp declares each texture and sampler heap as an OpTypeRuntimeArray in
// the UniformConstant storage class. That form requires the Vulkan 1.2
// runtimeDescriptorArray feature (and the SPV_EXT_descriptor_indexing
// extension), so a driver without it - the Mali r49 driver of the Poco C65 is
// one - rejects the module in vkCreateShaderModule even when the renderer
// itself no longer needs the feature: its cached descriptor backend binds
// fixed-size arrays and writes every descriptor of every binding before a draw
// (see ChooseNativeDescriptorBackend and CreateNativeDescriptors in
// graphics_system.cpp), which is what descriptorBindingPartiallyBound would
// otherwise allow to be skipped.
//
// The rewrite mirrors util::StripSpirvClipDistance: it walks the words and
// emits a new module. Each descriptor heap array becomes an OpTypeArray of the
// capacity the cached layout uses, together with the OpConstant and, when the
// module has none, the unsigned int type the length needs; the UniformConstant
// pointers that referenced the runtime array are retargeted at the new type,
// and the RuntimeDescriptorArray capability is dropped. A descriptor heap is
// identified by storage class, not by element type alone: a runtime array that
// is the last member of a storage buffer (the common way to declare an
// unsized buffer) has a different pointee storage class and is left alone,
// since it never needed the capability.
//
// The device still needs shaderSampledImageArrayDynamicIndexing, because the
// title shaders index the heaps with a runtime (dynamically uniform) index; the
// device check in vulkan_device.cpp requires it before this path is taken.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rex::graphics::gta4_native {

// Capacity of the fixed-size descriptor arrays the cached backend binds. Must
// equal the descriptorCount of the cached descriptor set layout's bindings,
// which is kShaderTextureCount (kTextureStageCount); graphics_system.cpp
// asserts the two stay equal.
constexpr uint32_t kNativeFixedDescriptorArrayCapacity = 26;

struct SpirvDescriptorArrayFixup {
  uint32_t arrays_rewritten = 0;
  uint32_t arrays_skipped = 0;
  bool dropped_capability = false;
  bool dropped_extension = false;
  bool changed() const {
    return arrays_rewritten != 0 || dropped_capability || dropped_extension;
  }
};

// A driver can key its module acceptance on the declared extensions rather than
// on the capabilities alone (the Mali blob compiles at pipeline creation and
// says nothing when it refuses), so the declaration is removed together with
// the feature that needed it. "SPV_EXT_descriptor_indexing" also enables
// NonUniform decorations; a module that keeps one keeps the declaration.
inline bool SpirvUsesDescriptorIndexingDecorations(const std::vector<uint32_t>& words) {
  constexpr uint16_t kOpDecorate = 71, kOpMemberDecorate = 72;
  constexpr uint32_t kDecorationNonUniform = 5300;
  if (words.size() < 5 || words[0] != 0x07230203u) return true;
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    if (!count || i + count > words.size()) return true;
    const uint16_t op = uint16_t(words[i] & 0xFFFF);
    const uint32_t* w = words.data() + i;
    if ((op == kOpDecorate && count >= 3 && w[2] == kDecorationNonUniform) ||
        (op == kOpMemberDecorate && count >= 4 && w[3] == kDecorationNonUniform)) {
      return true;
    }
    i += count;
  }
  return false;
}

// Converts every descriptor heap runtime array in `words` to an OpTypeArray of
// `capacity`. Leaves the module untouched (and reports no change) when the
// heaps cannot be classified unambiguously, so an unusual shader is passed
// through to the driver exactly as before rather than becoming invalid here.
inline SpirvDescriptorArrayFixup FixSpirvRuntimeDescriptorArrays(std::vector<uint32_t>& words,
                                                                 uint32_t capacity,
                                                                 bool strip_extension = true) {
  constexpr uint16_t kOpExtension = 10;
  constexpr uint16_t kOpName = 5, kOpCapability = 17, kOpTypeInt = 21, kOpTypeImage = 25,
                     kOpTypeSampler = 26, kOpTypeSampledImage = 27, kOpTypeArray = 28,
                     kOpTypeRuntimeArray = 29, kOpTypeStruct = 30, kOpTypePointer = 32,
                     kOpConstant = 43, kOpDecorate = 71, kOpMemberDecorate = 72;
  constexpr uint32_t kStorageClassUniformConstant = 0;
  constexpr uint32_t kCapabilityRuntimeDescriptorArray = 5302;
  static constexpr char kDescriptorIndexingExtension[] = "SPV_EXT_descriptor_indexing";
  const auto is_descriptor_indexing_extension = [](const uint32_t* w, uint32_t count) {
    constexpr size_t kNameLength = sizeof(kDescriptorIndexingExtension) - 1;
    if (count < 1 + (kNameLength + 1 + 3) / 4) return false;
    const auto* name = reinterpret_cast<const char*>(w + 1);
    return std::memcmp(name, kDescriptorIndexingExtension, kNameLength) == 0 &&
           name[kNameLength] == '\0';
  };

  SpirvDescriptorArrayFixup result{};
  if (words.size() < 5 || words[0] != 0x07230203u || !capacity) return result;

  // First pass: the type-level structure the rewrite needs, and the capability.
  uint32_t bound = words[3];
  uint32_t uint_type = 0;
  bool has_capability = false;
  bool has_extension = false;
  std::unordered_map<uint32_t, uint32_t> type_op;                        // result id -> OpType*
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;  // id -> {storage, pointee}
  std::unordered_map<uint32_t, std::pair<size_t, uint32_t>> runtime_arrays;  // id -> {index, element}
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    const uint16_t op = uint16_t(words[i] & 0xFFFF);
    if (!count || i + count > words.size()) return result;
    const uint32_t* w = words.data() + i;
    switch (op) {
      case kOpCapability:
        if (count >= 2 && w[1] == kCapabilityRuntimeDescriptorArray) has_capability = true;
        break;
      case kOpExtension:
        if (is_descriptor_indexing_extension(w, count)) has_extension = true;
        break;
      case kOpTypeInt:
        // The array length needs an unsigned 32-bit type; reuse the module's.
        if (count >= 4 && w[2] == 32 && w[3] == 0 && !uint_type) uint_type = w[1];
        break;
      case kOpTypeImage:
      case kOpTypeSampler:
      case kOpTypeSampledImage:
        type_op[w[1]] = op;
        break;
      case kOpTypeRuntimeArray:
        if (count >= 3) runtime_arrays.emplace(w[1], std::make_pair(i, w[2]));
        break;
      case kOpTypePointer:
        if (count >= 4) pointers.emplace(w[1], std::make_pair(w[2], w[3]));
        break;
      default:
        break;
    }
    i += count;
  }

  // Dropping the declaration is only valid when nothing else in the module
  // relies on the extension.
  const bool drop_extension =
      strip_extension && !SpirvUsesDescriptorIndexingDecorations(words);

  // A descriptor heap is an array reached through a UniformConstant pointer.
  // Anything else (a storage buffer's unsized last member, most often) is core
  // shader functionality and must be kept as it is.
  std::unordered_set<uint32_t> descriptor_arrays;
  bool unsupported = false;
  for (const auto& pointer : pointers) {
    const uint32_t storage_class = pointer.second.first;
    if (storage_class != kStorageClassUniformConstant) continue;
    const auto array = runtime_arrays.find(pointer.second.second);
    if (array == runtime_arrays.end()) continue;
    const auto element = type_op.find(array->second.second);
    if (element != type_op.end() &&
        (element->second == kOpTypeImage || element->second == kOpTypeSampler ||
         element->second == kOpTypeSampledImage)) {
      descriptor_arrays.insert(array->first);
    } else {
      // A runtime array of some other descriptor kind. Nothing in the title
      // uses one today; refuse to guess rather than drop the capability while
      // it still needs it.
      unsupported = true;
    }
  }

  const auto build_without_capability = [&]() {
    std::vector<uint32_t> out;
    out.reserve(words.size());
    out.insert(out.end(), words.begin(), words.begin() + 5);
    for (size_t i = 5; i < words.size();) {
      const uint32_t count = words[i] >> 16;
      if (!count || i + count > words.size()) return false;
      if ((words[i] & 0xFFFF) == kOpCapability && count >= 2 &&
          words[i + 1] == kCapabilityRuntimeDescriptorArray) {
        i += count;
        continue;
      }
      if (drop_extension && (words[i] & 0xFFFF) == kOpExtension &&
          is_descriptor_indexing_extension(words.data() + i, count)) {
        result.dropped_extension = true;
        i += count;
        continue;
      }
      out.insert(out.end(), words.begin() + i, words.begin() + i + count);
      i += count;
    }
    out[3] = bound;
    words.swap(out);
    return true;
  };

  if (descriptor_arrays.empty()) {
    // No heap to convert. The capability alone still makes some drivers reject
    // the module, so drop it when it is present. Runtime arrays that are not
    // descriptor heaps (a storage buffer's unsized last member) never required
    // it, so they do not keep it alive.
    if (unsupported) return result;
    if (!has_capability && !(drop_extension && has_extension)) return result;
    if (build_without_capability()) result.dropped_capability = has_capability;
    return result;
  }
  if (unsupported) return result;

  // A runtime array reachable both as a heap and as, say, a structure member
  // cannot be deleted, and deleting only some heaps would leave the capability
  // doing real work. Pass such a module through unchanged.
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    const uint16_t op = uint16_t(words[i] & 0xFFFF);
    if (!count || i + count > words.size()) return result;
    const uint32_t* w = words.data() + i;
    if (op == kOpTypeStruct) {
      for (uint32_t member = 2; member < count; ++member)
        if (descriptor_arrays.count(w[member])) result.arrays_skipped = 1;
    } else if (op == kOpTypeArray || op == kOpTypeRuntimeArray) {
      if (count >= 3 && descriptor_arrays.count(w[2])) result.arrays_skipped = 1;
    } else if (op == kOpTypePointer) {
      if (count >= 4 && w[2] != kStorageClassUniformConstant && descriptor_arrays.count(w[3]))
        result.arrays_skipped = 1;
    }
    i += count;
  }
  if (result.arrays_skipped) return result;

  // Second pass: emit the fixed-size replacements and retarget the pointers.
  std::vector<uint32_t> out;
  out.reserve(words.size() + 3 * descriptor_arrays.size() + 4);
  out.insert(out.end(), words.begin(), words.begin() + 5);
  auto emit_to = [](std::vector<uint32_t>& to, uint16_t op,
                    std::initializer_list<uint32_t> operands) {
    to.push_back(uint32_t(operands.size() + 1) << 16 | op);
    to.insert(to.end(), operands);
  };
  std::unordered_map<uint32_t, uint32_t> replacement;  // runtime array id -> fixed array type id
  uint32_t length_constant = 0;
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    const uint16_t op = uint16_t(words[i] & 0xFFFF);
    const uint32_t* w = words.data() + i;
    if (op == kOpCapability && count >= 2 && w[1] == kCapabilityRuntimeDescriptorArray) {
      result.dropped_capability = true;
      i += count;
      continue;
    }
    if (drop_extension && op == kOpExtension &&
        is_descriptor_indexing_extension(w, count)) {
      result.dropped_extension = true;
      i += count;
      continue;
    }
    // Annotations naming a type that is about to disappear would be orphaned.
    if ((op == kOpName || op == kOpDecorate || op == kOpMemberDecorate) && count >= 2 &&
        descriptor_arrays.count(w[1])) {
      i += count;
      continue;
    }
    if (op == kOpTypeRuntimeArray && count >= 3 && descriptor_arrays.count(w[1])) {
      if (!uint_type) {
        uint_type = bound++;
        emit_to(out, kOpTypeInt, {uint_type, 32, 0});
      }
      if (!length_constant) {
        length_constant = bound++;
        emit_to(out, kOpConstant, {uint_type, length_constant, capacity});
      }
      const uint32_t array_type = bound++;
      emit_to(out, kOpTypeArray, {array_type, w[2], length_constant});
      replacement.emplace(w[1], array_type);
      ++result.arrays_rewritten;
      i += count;
      continue;
    }
    if (op == kOpTypePointer && count >= 4) {
      const auto fixed = replacement.find(w[3]);
      if (fixed != replacement.end()) {
        emit_to(out, kOpTypePointer, {w[1], w[2], fixed->second});
        i += count;
        continue;
      }
    }
    out.insert(out.end(), w, w + count);
    i += count;
  }
  out[3] = bound;
  words.swap(out);
  return result;
}

}  // namespace rex::graphics::gta4_native
