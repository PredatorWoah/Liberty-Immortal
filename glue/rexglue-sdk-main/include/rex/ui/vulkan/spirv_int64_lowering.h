// Mali Edition: 64-bit integer lowering for GPUs without shaderInt64.
//
// The title shaders use 64-bit integers only for buffer device addresses: an
// address is loaded from the push constants, offset with 64-bit adds, and
// bitcast to a PhysicalStorageBuffer pointer, plus one bit test (and, shift,
// not-equal). This pass rewrites every 64-bit integer value as a uvec2 (low,
// high) and each of those operations as 32-bit arithmetic on the halves (an
// add with carry, component-wise and, a 64-bit shift built from 32-bit shifts,
// a two-half compare). A uvec2 can be bitcast to a PhysicalStorageBuffer
// pointer directly, so the module ends up without the Int64 capability.
//
// Anything outside that set (64-bit vectors, other operations, function
// parameters, ...) makes the pass give up and leave the module unchanged.
#pragma once

#include <cstddef>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::ui::vulkan::spirv_int64 {

namespace op {
constexpr uint32_t Name = 5, MemberName = 6, Extension = 10, ExtInstImport = 11,
                   MemoryModel = 14, EntryPoint = 15, ExecutionMode = 16, Capability = 17,
                   TypeBool = 20, TypeInt = 21, TypeVector = 23, TypeStruct = 30,
                   TypePointer = 32, TypeFunction = 33, Constant = 43, ConstantComposite = 44,
                   ConstantNull = 46, SpecConstant = 50, Function = 54, FunctionParameter = 55,
                   FunctionEnd = 56, Variable = 59, Load = 61, Store = 62, Decorate = 71,
                   MemberDecorate = 72, CompositeConstruct = 80, CompositeExtract = 81,
                   CopyObject = 83, SConvert = 114, UConvert = 113, Bitcast = 124, IAdd = 128,
                   ISub = 130, IAddCarry = 149, LogicalOr = 166, Select = 169, IEqual = 170,
                   INotEqual = 171, UGreaterThanEqual = 174, LogicalAnd = 167, ShiftRightLogical = 194,
                   ShiftRightArithmetic = 195, ShiftLeftLogical = 196, BitwiseOr = 197,
                   BitwiseAnd = 199, ConvertUToPtr = 120, Line = 8, NoLine = 317, Label = 248;
}  // namespace op
constexpr uint32_t kCapabilityInt64 = 11;
constexpr uint32_t kMagic = 0x07230203;

struct Inst {
  uint32_t opcode;
  std::vector<uint32_t> words;  // full instruction, words[0] = (count << 16) | opcode
};

// Whether word `w` of a function-body instruction is an <id> operand (as
// opposed to a literal), for the opcodes that carry literals. Unknown opcodes
// count every word as an id, which can only make the pass give up, never
// rewrite wrongly.
inline bool IsIdOperand(const std::vector<uint32_t>& words, uint32_t opcode, size_t w) {
  switch (opcode) {
    case 8:    // OpLine: file id, line, column
      return w == 1;
    case 12:   // OpExtInst: type, result, set, instruction (literal), operands
      return w != 4;
    case 81:   // OpCompositeExtract: indices are literals
      return w < 4;
    case 82:   // OpCompositeInsert
      return w < 5;
    case 79:   // OpVectorShuffle: components are literals
      return w < 5;
    case 61:   // OpLoad: memory operands
      return w < 4;
    case 62:   // OpStore
      return w < 3;
    case 247:  // OpSelectionMerge: merge block, control
      return w == 1;
    case 246:  // OpLoopMerge: merge, continue, control...
      return w <= 2;
    case 250:  // OpBranchConditional: weights are literals
      return w <= 3;
    case 251:  // OpSwitch: selector, default, then (literal, label) pairs
      return w <= 2 || (w >= 3 && (w - 3) % 2 == 1);
    default:
      // Image instructions (OpImageSample* .. OpImageWrite, OpImageSparse*):
      // the image-operands mask after the coordinate is a literal.
      if ((opcode >= 87 && opcode <= 99) || (opcode >= 305 && opcode <= 315)) return w != 5;
      return true;
  }
}

inline Inst Make(uint32_t opcode, std::initializer_list<uint32_t> operands) {
  Inst inst;
  inst.opcode = opcode;
  inst.words.reserve(operands.size() + 1);
  inst.words.push_back(uint32_t(operands.size() + 1) << 16 | opcode);
  inst.words.insert(inst.words.end(), operands.begin(), operands.end());
  return inst;
}

// Rewrites `code`. Returns false (and leaves `out` alone) when the module has
// no 64-bit integers or uses them in a way this pass does not handle; `why`
// then says which (for logs).
inline bool Lower(const uint32_t* code, size_t word_count, std::vector<uint32_t>& out,
                  std::string* why = nullptr) {
  auto fail = [&](const std::string& reason) {
    if (why) *why = reason;
    return false;
  };
  if (word_count < 5 || code[0] != kMagic) return fail("not SPIR-V");

  // ---- parse
  std::vector<Inst> insts;
  for (size_t i = 5; i < word_count;) {
    const uint32_t count = code[i] >> 16;
    if (count == 0 || i + count > word_count) return fail("malformed instruction");
    Inst inst;
    inst.opcode = code[i] & 0xFFFF;
    inst.words.assign(code + i, code + i + count);
    insts.push_back(std::move(inst));
    i += count;
  }
  uint32_t bound = code[3];
  auto fresh = [&]() { return bound++; };

  // ---- find the 64-bit integer types and related declarations
  std::unordered_set<uint32_t> i64_types;  // OpTypeInt 64 (signed or unsigned)
  uint32_t uint_type = 0, bool_type = 0;
  size_t first_i64_index = SIZE_MAX, uvec2_index = SIZE_MAX, uint_index = SIZE_MAX;
  uint32_t existing_uvec2 = 0;
  bool has_int64_capability = false;
  for (const Inst& inst : insts) {
    if (inst.opcode == op::Capability && inst.words[1] == kCapabilityInt64) has_int64_capability = true;
    if (inst.opcode == op::TypeInt && inst.words[2] == 64) {
      i64_types.insert(inst.words[1]);
      first_i64_index = std::min(first_i64_index, size_t(&inst - insts.data()));
    }
    if (inst.opcode == op::TypeInt && inst.words[2] == 32 && inst.words[3] == 0) {
      uint_type = inst.words[1];
      uint_index = size_t(&inst - insts.data());
    }
    if (inst.opcode == op::TypeBool) bool_type = inst.words[1];
  }
  if (i64_types.empty()) return fail("no 64-bit integers");
  if (!uint_type) return fail("no 32-bit unsigned integer type");
  for (size_t i = 0; i < insts.size(); ++i) {
    const Inst& inst = insts[i];
    if (inst.opcode == op::TypeVector && inst.words[2] == uint_type && inst.words[3] == 2) {
      existing_uvec2 = inst.words[1];
      uvec2_index = i;
      break;
    }
  }
  // An existing uvec2 declared after the 64-bit type moves up to it; one
  // declared before stays where it is.
  const bool reuse_uvec2_in_place = existing_uvec2 && uvec2_index < first_i64_index;
  const bool move_uvec2 = existing_uvec2 && uvec2_index > first_i64_index;

  // Every value of a 64-bit type becomes a uvec2 value.
  std::unordered_set<uint32_t> values;
  // Type position bookkeeping: declarations that refer to a 64-bit type.
  for (const Inst& inst : insts) {
    switch (inst.opcode) {
      case op::TypeVector:
        if (i64_types.count(inst.words[2])) return fail("64-bit integer vector");
        break;
      case op::TypeFunction:
        for (size_t w = 2; w < inst.words.size(); ++w)
          if (i64_types.count(inst.words[w])) return fail("64-bit integer in a function type");
        break;
      case op::SpecConstant:
      case op::ConstantNull:
        if (i64_types.count(inst.words[1])) return fail("64-bit spec/null constant");
        break;
      case op::FunctionParameter:
      case op::Variable:
        if (i64_types.count(inst.words[1])) return fail("64-bit variable or parameter");
        break;
      default:
        break;
    }
  }

  // ---- new declarations, inserted where the first 64-bit type was declared
  const uint32_t uvec2_type = existing_uvec2 ? existing_uvec2 : fresh();
  // A uint declared after the 64-bit type moves up to it (it depends on nothing).
  const bool move_uint = uint_index > first_i64_index;
  const uint32_t carry_struct = fresh();
  const bool need_bool = bool_type == 0;
  if (need_bool) bool_type = fresh();
  std::vector<Inst> new_decls;
  if (move_uint) new_decls.push_back(Make(op::TypeInt, {uint_type, 32, 0}));
  if (!reuse_uvec2_in_place) new_decls.push_back(Make(op::TypeVector, {uvec2_type, uint_type, 2}));
  new_decls.push_back(Make(op::TypeStruct, {carry_struct, uint_type, uint_type}));
  if (need_bool) new_decls.push_back(Make(op::TypeBool, {bool_type}));
  std::unordered_map<uint32_t, uint32_t> uint_constants;
  auto uint_constant = [&](uint32_t value) {
    auto it = uint_constants.find(value);
    if (it != uint_constants.end()) return it->second;
    const uint32_t id = fresh();
    new_decls.push_back(Make(op::Constant, {uint_type, id, value}));
    uint_constants.emplace(value, id);
    return id;
  };
  const uint32_t c0 = uint_constant(0), c31 = uint_constant(31), c32 = uint_constant(32),
                 c63 = uint_constant(63);

  // ---- rewrite
  std::vector<Inst> result;
  result.reserve(insts.size() * 2);
  bool inserted = false, seen_uint = false;
  size_t insert_at = 0;
  auto insert_decls = [&]() {
    if (inserted) return;
    insert_at = result.size();  // new declarations are spliced in here at the end
    inserted = true;
  };
  bool in_function = false;
  for (size_t index = 0; index < insts.size(); ++index) {
    Inst inst = insts[index];
    const uint32_t opc = inst.opcode;
    if (opc == op::TypeInt && inst.words[1] == uint_type) seen_uint = true;

    // Module-level declarations.
    if (!in_function) {
      if (move_uvec2 && opc == op::TypeVector && inst.words[1] == existing_uvec2) continue;
      if (move_uint && opc == op::TypeInt && inst.words[1] == uint_type) continue;
      if (opc == op::Capability && inst.words[1] == kCapabilityInt64) continue;
      if (opc == op::Name || opc == op::MemberName) {
        if (i64_types.count(inst.words[1])) continue;  // the removed types' names
        result.push_back(inst);
        continue;
      }
      if (opc == op::TypeInt && i64_types.count(inst.words[1])) {
        insert_decls();
        continue;  // the 64-bit type itself is gone
      }
      if (opc == op::TypeStruct) {
        for (size_t w = 2; w < inst.words.size(); ++w)
          if (i64_types.count(inst.words[w])) {
            if (!inserted) return fail("struct before the 64-bit type");
            inst.words[w] = uvec2_type;
          }
        result.push_back(inst);
        continue;
      }
      if (opc == op::TypePointer) {
        if (i64_types.count(inst.words[3])) {
          if (!inserted) return fail("pointer before the 64-bit type");
          inst.words[3] = uvec2_type;
        }
        result.push_back(inst);
        continue;
      }
      if (opc == op::Constant && i64_types.count(inst.words[1])) {
        if (!inserted) return fail("constant before the 64-bit type");
        if (inst.words.size() != 5) return fail("64-bit constant with odd width");
        const uint32_t lo = uint_constant(inst.words[3]), hi = uint_constant(inst.words[4]);
        values.insert(inst.words[2]);
        result.push_back(Make(op::ConstantComposite, {uvec2_type, inst.words[2], lo, hi}));
        continue;
      }
      if (opc == op::ConstantComposite && i64_types.count(inst.words[1]))
        return fail("64-bit composite constant");
      if (opc == op::Function) in_function = true;
      result.push_back(inst);
      continue;
    }

    // Function bodies.
    if (opc == op::FunctionEnd) {
      in_function = false;
      result.push_back(inst);
      continue;
    }
    if (opc == op::Function) return fail("nested function");

    auto extract = [&](uint32_t composite, uint32_t lane) {
      const uint32_t id = fresh();
      result.push_back(Make(op::CompositeExtract, {uint_type, id, composite, lane}));
      return id;
    };
    auto emit = [&](uint32_t opcode, uint32_t type, std::initializer_list<uint32_t> args) {
      const uint32_t id = fresh();
      std::vector<uint32_t> ops{type, id};
      ops.insert(ops.end(), args.begin(), args.end());
      Inst made;
      made.opcode = opcode;
      made.words.push_back(uint32_t(ops.size() + 1) << 16 | opcode);
      made.words.insert(made.words.end(), ops.begin(), ops.end());
      result.push_back(made);
      return id;
    };
    const bool has_result_type = inst.words.size() >= 3;
    const bool produces = has_result_type && i64_types.count(inst.words[1]) &&
                          opc != op::Label && opc != op::Line;
    auto uses_value = [&](size_t from) {
      for (size_t w = from; w < inst.words.size(); ++w)
        if (IsIdOperand(inst.words, opc, w) && values.count(inst.words[w])) return true;
      return false;
    };

    if (produces) {
      const uint32_t res = inst.words[2];
      values.insert(res);
      switch (opc) {
        case op::Load:
          inst.words[1] = uvec2_type;  // the pointer now points at a uvec2
          result.push_back(inst);
          break;
        case op::IAdd: {
          const uint32_t a = inst.words[3], b = inst.words[4];
          if (!values.count(a) || !values.count(b)) return fail("64-bit add of a non-64-bit value");
          const uint32_t a0 = extract(a, 0), a1 = extract(a, 1), b0 = extract(b, 0), b1 = extract(b, 1);
          const uint32_t sum = emit(op::IAddCarry, carry_struct, {a0, b0});
          const uint32_t lo = extract(sum, 0), carry = extract(sum, 1);
          const uint32_t high = emit(op::IAdd, uint_type, {a1, b1});
          const uint32_t hi = emit(op::IAdd, uint_type, {high, carry});
          result.push_back(Make(op::CompositeConstruct, {uvec2_type, res, lo, hi}));
          break;
        }
        case op::UConvert: {
          const uint32_t x = inst.words[3];
          if (values.count(x)) return fail("64-to-64 UConvert");
          result.push_back(Make(op::CompositeConstruct, {uvec2_type, res, x, c0}));
          break;
        }
        case op::SConvert: {
          const uint32_t x = inst.words[3];
          if (values.count(x)) return fail("64-to-64 SConvert");
          // The source may be a signed int: reinterpret as uint first.
          const uint32_t lo = emit(op::Bitcast, uint_type, {x});
          const uint32_t hi = emit(op::ShiftRightArithmetic, uint_type, {lo, c31});
          result.push_back(Make(op::CompositeConstruct, {uvec2_type, res, lo, hi}));
          break;
        }
        case op::Bitcast: {
          const uint32_t x = inst.words[3];
          if (!values.count(x)) return fail("bitcast to 64-bit from a non-64-bit value");
          result.push_back(Make(op::CopyObject, {uvec2_type, res, x}));  // signedness only
          break;
        }
        case op::BitwiseAnd: {
          if (!values.count(inst.words[3]) || !values.count(inst.words[4]))
            return fail("64-bit and with a non-64-bit value");
          result.push_back(Make(op::BitwiseAnd, {uvec2_type, res, inst.words[3], inst.words[4]}));
          break;
        }
        case op::Select: {
          // Scalar condition: select each half (a vector select would need a
          // bvec2 condition before SPIR-V 1.4).
          const uint32_t cond = inst.words[3], a = inst.words[4], b = inst.words[5];
          if (!values.count(a) || !values.count(b)) return fail("64-bit select of a non-64-bit value");
          const uint32_t a0 = extract(a, 0), a1 = extract(a, 1), b0 = extract(b, 0), b1 = extract(b, 1);
          const uint32_t lo = emit(op::Select, uint_type, {cond, a0, b0});
          const uint32_t hi = emit(op::Select, uint_type, {cond, a1, b1});
          result.push_back(Make(op::CompositeConstruct, {uvec2_type, res, lo, hi}));
          break;
        }
        case op::ShiftLeftLogical: {
          const uint32_t base = inst.words[3], amount = inst.words[4];
          if (!values.count(base)) return fail("shift of a non-64-bit base");
          // The amount may be 64-bit (lowered) or 32-bit.
          const uint32_t raw = values.count(amount) ? extract(amount, 0) : amount;
          const uint32_t s = emit(op::BitwiseAnd, uint_type, {raw, c63});
          const uint32_t lo = extract(base, 0), hi = extract(base, 1);
          // s < 32
          const uint32_t lo_s = emit(op::ShiftLeftLogical, uint_type, {lo, s});
          const uint32_t hi_s = emit(op::ShiftLeftLogical, uint_type, {hi, s});
          const uint32_t inv = emit(op::ISub, uint_type, {c32, s});
          const uint32_t spill = emit(op::ShiftRightLogical, uint_type, {lo, inv});
          const uint32_t is_zero = emit(op::IEqual, bool_type, {s, c0});
          const uint32_t spill_safe = emit(op::Select, uint_type, {is_zero, c0, spill});
          const uint32_t hi_small = emit(op::BitwiseOr, uint_type, {hi_s, spill_safe});
          // s >= 32
          const uint32_t s_minus = emit(op::ISub, uint_type, {s, c32});
          const uint32_t hi_large = emit(op::ShiftLeftLogical, uint_type, {lo, s_minus});
          const uint32_t large = emit(op::UGreaterThanEqual, bool_type, {s, c32});
          const uint32_t new_lo = emit(op::Select, uint_type, {large, c0, lo_s});
          const uint32_t new_hi = emit(op::Select, uint_type, {large, hi_large, hi_small});
          result.push_back(Make(op::CompositeConstruct, {uvec2_type, res, new_lo, new_hi}));
          break;
        }
        default:
          return fail("unsupported 64-bit operation (opcode " + std::to_string(opc) + ")");
      }
      continue;
    }

    // Instructions that consume a 64-bit value without producing one.
    if (has_result_type && uses_value(3)) {
      switch (opc) {
        case op::Bitcast:
          // To a PhysicalStorageBuffer pointer: a uvec2 operand is valid.
          result.push_back(inst);
          break;
        case op::ConvertUToPtr:
          // An address in a uvec2 becomes a pointer through OpBitcast.
          result.push_back(Make(op::Bitcast, {inst.words[1], inst.words[2], inst.words[3]}));
          break;
        case op::INotEqual: {
          const uint32_t a = inst.words[3], b = inst.words[4];
          if (!values.count(a) || !values.count(b)) return fail("64-bit compare with a non-64-bit value");
          const uint32_t a0 = extract(a, 0), a1 = extract(a, 1), b0 = extract(b, 0), b1 = extract(b, 1);
          const uint32_t n0 = emit(op::INotEqual, bool_type, {a0, b0});
          const uint32_t n1 = emit(op::INotEqual, bool_type, {a1, b1});
          result.push_back(Make(op::LogicalOr, {inst.words[1], inst.words[2], n0, n1}));
          break;
        }
        case op::IEqual: {
          const uint32_t a = inst.words[3], b = inst.words[4];
          if (!values.count(a) || !values.count(b)) return fail("64-bit compare with a non-64-bit value");
          const uint32_t a0 = extract(a, 0), a1 = extract(a, 1), b0 = extract(b, 0), b1 = extract(b, 1);
          const uint32_t e0 = emit(op::IEqual, bool_type, {a0, b0});
          const uint32_t e1 = emit(op::IEqual, bool_type, {a1, b1});
          result.push_back(Make(op::LogicalAnd, {inst.words[1], inst.words[2], e0, e1}));
          break;
        }
        case op::UConvert: {
          // 64 -> 32 truncation: the low half.
          const uint32_t x = inst.words[3];
          result.push_back(Make(op::CompositeExtract, {inst.words[1], inst.words[2], x, 0}));
          break;
        }
        default:
          return fail("unsupported use of a 64-bit value (opcode " + std::to_string(opc) + ")");
      }
      continue;
    }
    if (!has_result_type && uses_value(1)) return fail("64-bit value in opcode " + std::to_string(opc));
    result.push_back(inst);
  }
  if (!has_int64_capability && !inserted) return fail("nothing to lower");
  if (!inserted) return fail("64-bit type never declared");

  result.insert(result.begin() + std::ptrdiff_t(insert_at), new_decls.begin(), new_decls.end());

  out.clear();
  out.reserve(word_count + result.size() * 4);
  out.insert(out.end(), code, code + 5);
  out[3] = bound;
  for (const Inst& inst : result) out.insert(out.end(), inst.words.begin(), inst.words.end());
  return true;
}

}  // namespace rex::ui::vulkan::spirv_int64
