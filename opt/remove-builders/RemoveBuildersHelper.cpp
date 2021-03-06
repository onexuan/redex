/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RemoveBuildersHelper.h"

#include <boost/dynamic_bitset.hpp>
#include <boost/regex.hpp>

#include "ControlFlow.h"
#include "Dataflow.h"
#include "DexUtil.h"
#include "Transform.h"

namespace {

void fields_mapping(const DexInstruction* insn,
                    FieldsRegs* fregs,
                    DexClass* builder,
                    bool is_setter) {
  // Check if the register that used to hold the field's value is overwritten.
  if (insn->dests_size()) {
    const int current_dest = insn->dest();

    for (const auto& pair : fregs->field_to_reg) {
      if (pair.second == current_dest) {
        fregs->field_to_reg[pair.first] = FieldOrRegStatus::OVERWRITTEN;
      }

      if (insn->dest_is_wide()) {
        if (pair.second == current_dest + 1) {
          fregs->field_to_reg[pair.first] = FieldOrRegStatus::OVERWRITTEN;
        }
      }
    }
  }

  if ((is_setter && is_iput(insn->opcode())) ||
      (!is_setter && is_iget(insn->opcode()))) {
    auto field = static_cast<const DexOpcodeField*>(insn)->field();

    if (field->get_class() == builder->get_type()) {
      uint16_t current = is_setter ? insn->src(0) : insn->dest();
      fregs->field_to_reg[field] = current;
    }
  }
}

/**
 * Returns for every instruction, field value:
 * - a register: representing the register that stores the field's value
 * - UNDEFINED: not defined yet.
 * - DIFFERENT: no unique register.
 * - OVERWRITTEN: register no longer holds the value.
 */
std::unique_ptr<std::unordered_map<DexInstruction*, FieldsRegs>>
fields_setters(const std::vector<Block*>& blocks,
               DexClass* builder) {

  std::function<void(const DexInstruction*, FieldsRegs*)> trans = [&](
      const DexInstruction* insn, FieldsRegs* fregs) {
    fields_mapping(insn, fregs, builder, true);
  };

  return forwards_dataflow(blocks, FieldsRegs(builder), trans);
}

/**
 * Returns for every instruction, field value:
 * - a register: representing the register that had the field's value.
 * - UNDEFINED: not defined yet (no getter).
 * - DIFFERENT: different registers used to hold the field's value.
 * - OVERWRITTEN: register overwritten.
 */
std::unique_ptr<std::unordered_map<DexInstruction*, FieldsRegs>>
fields_getters(const std::vector<Block*>& blocks,
               DexClass* builder) {

  std::function<void(const DexInstruction*, FieldsRegs*)> trans = [&](
      const DexInstruction* insn, FieldsRegs* fregs) {
    fields_mapping(insn, fregs, builder, false);
  };

  return forwards_dataflow(blocks, FieldsRegs(builder), trans);
}


/**
 * Adds an instruction that initializes a new register with null.
 * Return if the operation succeeded or not.
 */
bool add_null_instr(DexMethod* method, MethodTransform* transform) {
  always_assert(method != nullptr);
  always_assert(transform != nullptr);

  auto& code = method->get_code();
  auto oldregs = code->get_registers_size();
  auto ins = code->get_ins_size();
  auto newregs = oldregs + 1;

  if (!MethodTransform::enlarge_regs(method, newregs)) {
    return false;
  }

  DexInstruction* insn = new DexInstruction(OPCODE_CONST_4);

  // Using last non-input register, since it was freed.
  uint16_t last_non_input_reg = oldregs - ins;
  insn->set_dest(last_non_input_reg);
  insn->set_literal(0);

  std::vector<DexInstruction*> insns;
  insns.push_back(insn);

  // Adds the instruction at the beginning, since it might be
  // used in various places later.
  transform->insert_after(nullptr, insns);

  return true;
}

using ReplacementsList =
  std::vector<std::tuple<DexInstruction*, size_t, uint16_t>>;

bool treat_undefined_fields(
    DexMethod* method,
    const std::vector<std::tuple<DexInstruction*, size_t>>&
      undefined_replacements,
    ReplacementsList* replacements) {

  const bool has_undefined = undefined_replacements.size() > 0;

  if (has_undefined) {

    auto& code = method->get_code();
    auto transform = code->get_entries();

    auto regs = code->get_registers_size();
    auto ins = code->get_ins_size();
    auto non_input_regs = regs - ins;

    if (!add_null_instr(method, transform)) {
      return false;
    }

    for (auto& insn_replace : *replacements) {
      uint16_t new_reg = std::get<2>(insn_replace);

      if (has_undefined && new_reg >= non_input_regs) {
        std::get<2>(insn_replace) = new_reg + 1;
      }
    }

    for (const auto& insn_replace : undefined_replacements) {
      replacements->emplace_back(
          std::get<0>(insn_replace),
          std::get<1>(insn_replace),
          non_input_regs);
    }
  }

  return true;
}

void method_updates(
    DexMethod* method,
    const std::vector<DexInstruction*>& deletes,
    const ReplacementsList& replacements) {

  auto& code = method->get_code();
  auto transform = code->get_entries();

  for (const auto& insn : deletes) {
    transform->remove_opcode(insn);
  }

  for (const auto& insn_replace : replacements) {
    DexInstruction* insn = std::get<0>(insn_replace);
    size_t index = std::get<1>(insn_replace);
    uint16_t new_reg = std::get<2>(insn_replace);

    insn->set_src(index, new_reg);
  }
}

}  // namespace

///////////////////////////////////////////////

void TaintedRegs::meet(const TaintedRegs& that) {
  m_reg_set |= that.m_reg_set;
}

bool TaintedRegs::operator==(const TaintedRegs& that) const {
  return m_reg_set == that.m_reg_set;
}

bool TaintedRegs::operator!=(const TaintedRegs& that) const {
  return !(*this == that);
}

void FieldsRegs::meet(const FieldsRegs& that) {
  for (const auto& pair : field_to_reg) {
    if (field_to_reg.at(pair.first) != that.field_to_reg.at(pair.first)) {
      field_to_reg[pair.first] = FieldOrRegStatus::DIFFERENT;
    }
  }
}

bool FieldsRegs::operator==(const FieldsRegs& that) const {
  return field_to_reg == that.field_to_reg;
}

bool FieldsRegs::operator!=(const FieldsRegs& that) const {
  return !(*this == that);
}

//////////////////////////////////////////////

DexMethod* get_build_method(const std::vector<DexMethod*>& vmethods) {
  static auto build = DexString::make_string("build");
  for (const auto& vmethod : vmethods) {
    if (vmethod->get_name() == build) {
      return vmethod;
    }
  }

  return nullptr;
}

bool inline_build(DexMethod* method, DexClass* builder) {
  auto& code = method->get_code();
  if (!code) {
    return false;
  }

  std::vector<std::pair<DexMethod*, DexOpcodeMethod*>> inlinables;
  DexMethod* build_method = get_build_method(builder->get_vmethods());

  for (auto const& mie : InstructionIterable(code->get_entries())) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto invoked = static_cast<const DexOpcodeMethod*>(insn)->get_method();
      if (invoked == build_method) {
        auto mop = static_cast<DexOpcodeMethod*>(insn);
        inlinables.push_back(std::make_pair(build_method, mop));
      }
    }
  }

  // For the moment, not treating the case where we have 2 instances
  // of the same builder.
  if (inlinables.size() > 1) {
    return false;
  }

  InlineContext inline_context(method, false);
  for (auto inlinable : inlinables) {
    // TODO(emmasevastian): We will need to gate this with a check, mostly as
    //                      we loosen the build method restraints.
    if (!MethodTransform::inline_16regs(
          inline_context, inlinable.first, inlinable.second)) {
      return false;
    }
  }

  return true;
}

bool remove_builder(DexMethod* method, DexClass* builder, DexClass* buildee) {
  auto& code = method->get_code();
  if (!code) {
    return false;
  }

  auto transform = code->get_entries();
  transform->build_cfg();
  auto blocks = postorder_sort(transform->cfg().blocks());

  auto fields_in = fields_setters(blocks, builder);
  auto fields_out = fields_getters(blocks, builder);

  static auto init = DexString::make_string("<init>");

  std::vector<DexInstruction*> deletes;
  std::vector<std::tuple<DexInstruction*, size_t>> undefined_replacements;
  ReplacementsList replacements;

  for (auto& block : blocks) {
    for (auto& mie : *block) {
      if (mie.type != MFLOW_OPCODE) {
        continue;
      }

      auto insn = mie.insn;
      DexOpcode opcode = insn->opcode();

      if (is_iput(opcode) || is_iget(opcode)) {
        auto field = static_cast<const DexOpcodeField*>(insn)->field();
        if (field->get_class() == builder->get_type()) {
          deletes.push_back(insn);
          continue;
        }

      } else if (opcode == OPCODE_NEW_INSTANCE) {
        DexType* cls = static_cast<DexOpcodeType*>(insn)->get_type();
        if (type_class(cls) == builder) {
          deletes.push_back(insn);
          continue;
        }

      } else if (is_invoke(opcode)) {
        auto invoked = static_cast<const DexOpcodeMethod*>(insn)->get_method();
        if (invoked->get_class() == builder->get_type() &&
            invoked->get_name() == init) {
          deletes.push_back(insn);
          continue;
        }
      }

      auto& fields_in_insn = fields_in->at(mie.insn);
      auto& fields_out_insn = fields_out->at(mie.insn);

      if (insn->srcs_size()) {
        for (size_t index = 0; index < insn->srcs_size(); ++index) {
          const int current_src = insn->src(index);

          for (const auto& pair : fields_out_insn.field_to_reg) {
            if (pair.second == current_src) {

              // TODO(emmaevastian): Treat the case where no register holds
              //                     the current field's value.
              auto field_in_value = fields_in_insn.field_to_reg[pair.first];
              if (field_in_value < 0) {

                if (field_in_value == FieldOrRegStatus::UNDEFINED) {
                  // Will add a new register that holds 'null'. This new
                  // register will be used here.
                  undefined_replacements.emplace_back(insn, index);
                } else {
                  return false;
                }
              } else {
                replacements.emplace_back(
                    insn,
                    index,
                    fields_in_insn.field_to_reg[pair.first]);
              }
            }
          }
        }
      }
    }
  }

  if (!treat_undefined_fields(method, undefined_replacements, &replacements)) {
    return false;
  }

  method_updates(method, deletes,  replacements);
  return true;
}
