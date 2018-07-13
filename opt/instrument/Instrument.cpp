/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Instrument.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "Match.h"
#include "Show.h"
#include "Walkers.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * This pass performs instrumentation for dynamic (runtime) analysis.
 *
 * Analysis code, which should be a static public method, is written in Java.
 * Its class and method names are specified in the config. This pass then
 * inserts the method to points of interest. For a starting example, we
 * implement the "onMethodBegin" instrumentation.
 */

namespace {

static bool debug = false;

bool match_class_name(std::string cls_name,
                      const std::unordered_set<std::string>& set) {
  always_assert(cls_name.back() == ';');
  cls_name.back() = '/';
  size_t pos = cls_name.find('/', 0);
  while (pos != std::string::npos) {
    if (set.count(cls_name.substr(0, pos + 1))) {
      return true;
    }
    pos = cls_name.find('/', pos + 1);
  }
  return false;
}

// For example, "Lcom/facebook/debug/" is in the set. We match either
// "^Lcom/facebook/debug/*" or "^Lcom/facebook/debug;".
bool is_excluded(std::string cls_name,
                 const std::unordered_set<std::string>& set) {
  return match_class_name(cls_name, set);
}

// Check for inclusion in whitelist of methods/classes.
bool is_included(std::string method,
                 std::string cls_name,
                 const std::unordered_set<std::string>& set) {
  if (match_class_name(cls_name, set)) {
    return true;
  }
  // Check for method by its full name(Class_Name;Method_Name).
  return set.count(cls_name + method);
}

DexMethod* find_analysis_method(const DexClass& cls, const std::string& name) {
  auto dmethods = cls.get_dmethods();
  auto it =
      std::find_if(dmethods.begin(), dmethods.end(), [&name](DexMethod* m) {
        return name == m->get_name()->str();
      });
  return it == dmethods.end() ? nullptr : *it;
}

static size_t num_opcodes_bb(cfg::Block* block) {
  size_t result = 0;
  for (const auto& inst : InstructionIterable(block)) {
    ++result;
  }
  return result;
}

void instrument_on_bb_begin(DexMethod* method, DexMethod* on_bb_begin) {
  IRCode* code = method->get_code();
  if (code == nullptr) {
    return;
  }
  code->build_cfg();
  const auto& blocks = code->cfg().blocks();
  TRACE(INSTRUMENT, 5, "[%s] Number of Basic Blocks: %zu\n",
        SHOW(method->get_name()), blocks.size());
  if (blocks.size() == 1) {
    return;
  }
  auto method_name_hash =
      (int32_t)(std::hash<std::string>{}(method->get_deobfuscated_name()));
  for (cfg::Block* block : blocks) {
    // Individual Block can be identified by method name and block id. We can
    // use a hash value of method name and add it to block id to
    // generate a unique identifier.
    size_t block_id = method_name_hash + block->id();
    IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
    const_inst->set_literal(block_id);

    const auto reg_dest = code->allocate_temp();
    const_inst->set_dest(reg_dest);

    IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
    invoke_inst->set_method(on_bb_begin);
    invoke_inst->set_arg_word_count(1);
    invoke_inst->set_src(0, reg_dest);

    // Find where to insert the newy created instruction block.
    auto insert_point = std::find_if_not(
        block->begin(), block->end(), [&](const MethodItemEntry& mie) {
          return mie.type == MFLOW_FALLTHROUGH ||
                 (mie.type == MFLOW_OPCODE &&
                  opcode::is_internal(mie.insn->opcode()));
        });

    // We do not instrument a BB if :
    // 1. It only has MFLOW_FALLTHROUGH or internal instructions.
    // 2. BB has 1 in-degree and 1 out-degree.
    // 3. BB has 0 or 1 opcodes.
    if (insert_point == block->end() ||
        (block->preds().size() <= 1 && block->succs().size() <= 1) ||
        num_opcodes_bb(block) <= 1) {
      TRACE(INSTRUMENT, 5, "No instrumentation to block: %zu\n", block_id);
      return;
    }

    TRACE(INSTRUMENT, 5, "Adding instrumentation to block: %zu\n", block_id);
    code->insert_before(code->insert_before(insert_point, invoke_inst),
                        const_inst);

  } // End of block loop.
}

void instrument_onMethodBegin(DexMethod* method,
                              int index,
                              const DexMethod* onMethodBegin) {
  IRCode* code = method->get_code();
  assert(code != nullptr);

  IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
  const_inst->set_literal(index);
  const auto reg_dest = code->allocate_temp();
  const_inst->set_dest(reg_dest);

  IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_inst->set_method(const_cast<DexMethod*>(onMethodBegin));
  invoke_inst->set_arg_word_count(1);
  invoke_inst->set_src(0, reg_dest);

  // TODO: Consider using get_param_instructions.
  // Try to find a right insertion point: the entry point of the method.
  // We skip any fall throughs and IOPCODE_LOAD_PARRM*.
  auto insert_point = std::find_if_not(
      code->begin(), code->end(), [&](const MethodItemEntry& mie) {
        return mie.type == MFLOW_FALLTHROUGH ||
               (mie.type == MFLOW_OPCODE &&
                opcode::is_load_param(mie.insn->opcode()));
      });

  if (insert_point == code->end()) {
    // No load params. So just insert before the head.
    insert_point = code->begin();
  } else if (insert_point->type == MFLOW_DEBUG) {
    // Right after the load params, there could be DBG_SET_PROLOGUE_END.
    // Skip if there is a following POSITION, too. For example:
    // 1: OPCODE: IOPCODE_LOAD_PARAM_OBJECT v1
    // 2: OPCODE: IOPCODE_LOAD_PARAM_OBJECT v2
    // 3: DEBUG: DBG_SET_PROLOGUE_END
    // 4: POSITION: foo.java:42 (this might be optional.)
    // <== Instrumentation code will be inserted here.
    //
    std::advance(insert_point,
                 std::next(insert_point)->type != MFLOW_POSITION ? 1 : 2);
  } else {
    // Otherwise, insert_point can be used directly.
  }

  code->insert_before(code->insert_before(insert_point, invoke_inst),
                      const_inst);

  if (debug) {
    for (auto it = code->begin(); it != code->end(); ++it) {
      if (it == insert_point) {
        TRACE(INSTRUMENT, 9, "<==== insertion\n");
        TRACE(INSTRUMENT, 9, "%s\n", SHOW(*it));
        ++it;
        if (it != code->end()) {
          TRACE(INSTRUMENT, 9, "%s\n", SHOW(*it));
          ++it;
          if (it != code->end()) {
            TRACE(INSTRUMENT, 9, "%s\n", SHOW(*it));
          }
        }
        TRACE(INSTRUMENT, 9, "\n");
        break;
      }
      TRACE(INSTRUMENT, 9, "%s\n", SHOW(*it));
    }
  }
}

// Find a sequence of opcode that creates a static array. Patch the array size.
void patch_stat_array_size(DexClass& analysis_cls,
                           const char* array_name,
                           const int array_size) {
  DexMethod* clinit = analysis_cls.get_clinit();
  always_assert(clinit != nullptr);

  auto* code = clinit->get_code();
  bool patched = false;
  walk::matching_opcodes_in_block(
      *clinit,
      // Don't find OPCODE_CONST. It might be deduped with others, or changing
      // this const can affect other instructions. (Well, we might have a unique
      // const number though.) So, just create a new const load instruction.
      // LocalDCE can clean up the redundant instructions.
      std::make_tuple(/* m::is_opcode(OPCODE_CONST), */
                      m::is_opcode(OPCODE_NEW_ARRAY),
                      m::is_opcode(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT),
                      m::is_opcode(OPCODE_SPUT_OBJECT)),
      [&](DexMethod* method,
          cfg::Block*,
          const std::vector<IRInstruction*>& insts) {
        assert(method == clinit);
        if (insts[2]->get_field()->get_name()->str() != array_name) {
          return;
        }

        IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
        const_inst->set_literal(array_size);
        const auto reg_dest = code->allocate_temp();
        const_inst->set_dest(reg_dest);
        insts[0]->set_src(0, reg_dest);
        for (auto& mie : InstructionIterable(code)) {
          if (mie.insn == insts[0]) {
            code->insert_before(code->iterator_to(mie), const_inst);
            patched = true;
            return;
          }
        }
      });

  if (!patched) {
    std::cerr << "[InstrumentPass] error: cannot patch array size."
              << std::endl;
    std::cerr << show(clinit->get_code()) << std::endl;
    exit(1);
  }

  TRACE(INSTRUMENT, 2, "%s array was patched: %d\n", array_name, array_size);
}

void patch_method_count(DexClass& analysis_cls,
                        const char* field_name,
                        const int new_number) {
  DexMethod* clinit = analysis_cls.get_clinit();
  always_assert(clinit != nullptr);

  // Find the sput with the given field name.
  auto* code = clinit->get_code();
  IRInstruction* sput_inst = nullptr;
  IRList::iterator insert_point;
  for (auto& mie : InstructionIterable(code)) {
    auto* insn = mie.insn;
    if (insn->opcode() == OPCODE_SPUT &&
        insn->get_field()->get_name()->str() == field_name) {
      sput_inst = insn;
      insert_point = code->iterator_to(mie);
      break;
    }
  }

  // SPUT can be null if the original field value was encoded in the
  // static_values_off array. And consider simplifying using make_concrete.
  if (sput_inst == nullptr) {
    TRACE(INSTRUMENT, 2, "sput %s was deleted; creating it\n", field_name);
    sput_inst = new IRInstruction(OPCODE_SPUT);
    sput_inst->set_field(
        DexField::make_field(DexType::make_type(analysis_cls.get_name()),
                             DexString::make_string(field_name),
                             DexType::make_type("I")));
    insert_point =
        code->insert_after(code->get_param_instructions().end(), sput_inst);
  }

  // Create a new const instruction just like patch_stat_array_size.
  IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
  const_inst->set_literal(new_number);
  const auto reg_dest = code->allocate_temp();
  const_inst->set_dest(reg_dest);

  sput_inst->set_src(0, reg_dest);
  code->insert_before(insert_point, const_inst);
  TRACE(INSTRUMENT, 2, "%s was patched: %d\n", field_name, new_number);
}

void write_method_index_file(const std::string& file_name,
                             const std::vector<DexMethod*>& method_id_vector) {
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  for (size_t i = 0; i < method_id_vector.size(); ++i) {
    ofs << i + 1 << ", " << show(method_id_vector[i]) << std::endl;
  }
  TRACE(INSTRUMENT, 2, "method index file was written to: %s\n",
        file_name.c_str());
}
DexMethod* verify_instrumentation_method(const DexClass& cls,
                                         const std::string& method_name) {
  DexMethod* function_to_insert = find_analysis_method(cls, method_name);
  if (function_to_insert == nullptr) {
    std::cerr << "[InstrumentPass] error: cannot find " << method_name << " in "
              << show(cls) << std::endl;
    for (auto&& m : cls.get_dmethods()) {
      std::cerr << " " << show(m) << std::endl;
    }
    exit(1);
  }
  return function_to_insert;
}

} // namespace

void InstrumentPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& cfg,
                              PassManager& pm) {
  if (m_analysis_class_name.empty()) {
    std::cerr << "[InstrumentPass] error: empty analysis class name."
              << std::endl;
    exit(1);
  }

  // Get the analysis class.
  DexType* analysis_class_type =
      g_redex->get_type(DexString::get_string(m_analysis_class_name.c_str()));
  if (analysis_class_type == nullptr) {
    std::cerr << "[InstrumentPass] error: cannot find analysis class: "
              << m_analysis_class_name << std::endl;
    exit(1);
  }

  DexClass* analysis_cls = g_redex->type_class(analysis_class_type);
  always_assert(analysis_cls != nullptr);

  // Check whether the analysis class is in the primary dex. We use a heuristic
  // that looks the last 12 characters of the location of the given dex.
  auto dex_loc = analysis_cls->get_dex_location();
  if (dex_loc.size() < 12 /* strlen("/classes.dex") == 12 */ ||
      dex_loc.substr(dex_loc.size() - 12) != "/classes.dex") {
    std::cerr << "[InstrumentPass] Analysis class must be in the primary dex. "
                 "It was in "
              << dex_loc << std::endl;
    exit(1);
  }

  auto scope = build_class_scope(stores);

  if (m_instrumentation_strategy == "method_tracing") {
    DexMethod* onMethodBegin =
        verify_instrumentation_method(*analysis_cls, m_analysis_method_name);
    TRACE(INSTRUMENT,
          3,
          "Loaded analysis class: %s (%s)\n",
          m_analysis_class_name.c_str(),
          analysis_cls->get_dex_location().c_str());

    // Instrument and build the method id map, too.
    std::unordered_map<DexMethod*, int /*id*/> method_id_map;
    std::vector<DexMethod*> method_id_vector;
    int index = 0;
    int excluded = 0;
    walk::methods(scope, [&](DexMethod* method) {
      if (method->get_code() == nullptr) {
        return;
      }
      if (method == onMethodBegin || method == analysis_cls->get_clinit()) {
        ++excluded;
        TRACE(INSTRUMENT, 2, "Excluding analysis method: %s\n", SHOW(method));
        return;
      }
      const auto& cls_name = show(method->get_class());
      if (!m_whitelist.empty() &&
          !is_included(method->get_name()->str(), cls_name, m_whitelist)) {
        return;
      }

      // In case of a conflict, when an entry is present in both blacklist
      // and whitelist, the blacklist is given priority and the entry
      // is not instrumented. Even in cases where a method is present
      // in whitelist and corresponding class is blacklisted, the method
      // is not instrumented.
      if (is_excluded(cls_name, m_blacklist)) {
        ++excluded;
        TRACE(INSTRUMENT, 7, "Excluding: %s\n", SHOW(method));
        return;
      }

      assert(!method_id_map.count(method));
      method_id_map.emplace(method, ++index);
      method_id_vector.push_back(method);
      TRACE(INSTRUMENT, 5, "%d: %s\n", method_id_map.at(method), SHOW(method));

      // NOTE: Only for testing D8607258! We test the method index file is
      // safely uploaded. So we enabled this pass but prevent actual
      // instrumentation.
      //
      // instrument_onMethodBegin(method, index * m_num_stats_per_method,
      //                         onMethodBegin);
    });

    TRACE(INSTRUMENT,
          1,
          "%d methods were instrumented (%d methods were excluded)\n",
          index,
          excluded);

    // Patch stat array size.
    patch_stat_array_size(*analysis_cls, "sStats",
                          index * m_num_stats_per_method);
    // Patch method count constant.
    patch_method_count(*analysis_cls, "sMethodCount", index);

    write_method_index_file(cfg.metafile(m_method_index_file_name),
                            method_id_vector);

    pm.incr_metric("Instrumented", index);
    pm.incr_metric("Excluded", excluded);
  } else if (m_instrumentation_strategy == "basic_block_tracing") {
    TRACE(INSTRUMENT, 5, "Basic Block Instrumentation begins here.\n");
    DexMethod* on_bb_begin =
        verify_instrumentation_method(*analysis_cls, m_analysis_method_name);

    // For each indivdual basic block from every method, assign them an
    // identifier and add a jump to on_bb_begin() at the beginning.
    // on_bb_begin() will set the touch variable when a basic block is accessed
    // at runtime.
    walk::methods(scope, [&](DexMethod* method) {
      if (method == on_bb_begin || method == analysis_cls->get_clinit()) {
        return;
      }
      const auto& cls_name = show(method->get_class());
      if (!m_whitelist.empty() &&
          !is_included(method->get_name()->str(), cls_name, m_whitelist)) {
        return;
      }
      instrument_on_bb_begin(method, on_bb_begin);
    });

  } else {
    std::cerr << "[InstrumentPass] Unknown option.\n";
  }
}

static InstrumentPass s_pass;