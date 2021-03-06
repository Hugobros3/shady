#include "list.h"
#include "dict.h"

#include "../log.h"
#include "../portability.h"
#include "../type.h"
#include "../analysis/scope.h"

#include "spirv_builder.h"

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

typedef struct SpvFileBuilder* FileBuilder;
typedef struct SpvFnBuilder* FnBuilder;
typedef struct SpvBasicBlockBuilder* BBBuilder;

typedef struct Emitter_ {
    IrArena* arena;
    CompilerConfig* configuration;
    FileBuilder file_builder;
    SpvId void_t;
    struct Dict* node_ids;
} Emitter;

SpvStorageClass emit_addr_space(AddressSpace address_space) {
    switch(address_space) {
        case AsGlobalLogical:   return SpvStorageClassStorageBuffer;
        case AsSharedLogical:   return SpvStorageClassCrossWorkgroup;
        case AsPrivateLogical:  return SpvStorageClassPrivate;
        case AsFunctionLogical: return SpvStorageClassFunction;

        case AsGeneric: error("not implemented");
        case AsGlobalPhysical: return SpvStorageClassPhysicalStorageBuffer;
        case AsSharedPhysical:
        case AsSubgroupPhysical:
        case AsPrivatePhysical: error("This should have been lowered before");

        case AsInput: return SpvStorageClassInput;
        case AsOutput: return SpvStorageClassOutput;

        // TODO: depending on platform, use push constants/ubos/ssbos here
        case AsExternal: return SpvStorageClassStorageBuffer;
        default: SHADY_NOT_IMPLEM;
    }
}

static SpvId emit_type(Emitter* emitter, const Type* type);
static SpvId emit_value(Emitter* emitter, const Node* node, const SpvId* use_id);

typedef struct {
    SpvId continue_target, break_target, join_target;
} MergeTargets;

static void emit_block(Emitter* emitter, FnBuilder fn_builder, BBBuilder basic_block_builder, MergeTargets, const Node* node);

static void register_result(Emitter* emitter, const Node* variable, SpvId id) {
    spvb_name(emitter->file_builder, id, variable->payload.var.name);
    insert_dict_and_get_result(struct Node*, SpvId, emitter->node_ids, variable, id);
}

enum OperandKind {
    Signed, Unsigned, Float, Logical, Ptr, Other, OperandKindsCount
};

enum ResultKind {
    Same, Bool, TyOperand
};

enum OperandKind static classify_operand(const Node* operand) {
    if (!is_type(operand))
        operand = operand->type;

    switch (without_qualifier(operand)->tag) {
        case Int_TAG:     return Signed;
        case Bool_TAG:    return Logical;
        case PtrType_TAG: return Ptr;
        // TODO float, unsigned
        default: error("we don't know what to do with this")
    }
}

/// What is considered when searching for an instruction
enum ISelMechanism {
    Custom, FirstOp, FirstAndResult
};

#define ISEL_IDENT 0 /* no-op, should be lowered to nothing beforehand */
#define ISEL_BOOLC 0 /* boolean conversions don't exist as a single instruction, a pass should lower them instead */
#define ISEL_ILLEG 0 /* doesn't make sense to support */

const struct IselTableEntry {
    enum ISelMechanism i_sel_mechanism;
    enum ResultKind result_kind;
    union {
        // matches first operand
        SpvOp fo[OperandKindsCount];
        // matches first operand and return type [first operand][result type]
        SpvOp foar[OperandKindsCount][OperandKindsCount];
    };
} isel_table[] = {
    [add_op] = { FirstOp, Same, .fo = { SpvOpIAdd, SpvOpIAdd, SpvOpFAdd }},
    [sub_op] = { FirstOp, Same, .fo = { SpvOpISub, SpvOpISub, SpvOpFSub }},
    [mul_op] = { FirstOp, Same, .fo = { SpvOpIMul, SpvOpIMul, SpvOpFMul }},
    [div_op] = { FirstOp, Same, .fo = { SpvOpSDiv, SpvOpUDiv, SpvOpFDiv }},
    [mod_op] = { FirstOp, Same, .fo = { SpvOpSMod, SpvOpUMod, SpvOpFMod }},

    [neg_op] = { FirstOp, Same, .fo = { SpvOpSNegate, SpvOpSNegate }},

    [eq_op]  = { FirstOp, Bool, .fo = { SpvOpIEqual,            SpvOpIEqual,            SpvOpFOrdNotEqual,         SpvOpLogicalEqual   }},
    [neq_op] = { FirstOp, Bool, .fo = { SpvOpINotEqual,         SpvOpINotEqual,         SpvOpFOrdNotEqual,         SpvOpLogicalNotEqual}},
    [lt_op]  = { FirstOp, Bool, .fo = { SpvOpSLessThan,         SpvOpULessThan,         SpvOpFOrdLessThan,         ISEL_ILLEG          }},
    [lte_op] = { FirstOp, Bool, .fo = { SpvOpSLessThanEqual,    SpvOpULessThanEqual,    SpvOpFOrdLessThanEqual,    ISEL_ILLEG          }},
    [gt_op]  = { FirstOp, Bool, .fo = { SpvOpSGreaterThan,      SpvOpUGreaterThan,      SpvOpFOrdGreaterThan,      ISEL_ILLEG          }},
    [gte_op] = { FirstOp, Bool, .fo = { SpvOpSGreaterThanEqual, SpvOpUGreaterThanEqual, SpvOpFOrdGreaterThanEqual, ISEL_ILLEG          }},

    [not_op] = { FirstOp, Same, .fo = { SpvOpNot,        SpvOpNot,        SpvOpLogicalNot }},
    [and_op] = { FirstOp, Same, .fo = { SpvOpBitwiseAnd, SpvOpBitwiseAnd, SpvOpLogicalAnd }},
    [or_op]  = { FirstOp, Same, .fo = { SpvOpBitwiseOr,  SpvOpBitwiseOr,  SpvOpLogicalOr }},
    [xor_op] = { FirstOp, Same, .fo = { SpvOpBitwiseXor, SpvOpBitwiseXor, SpvOpLogicalNotEqual }},

    [convert_op] = { FirstAndResult, TyOperand, .foar = {
        { SpvOpSConvert,    SpvOpUConvert,    SpvOpConvertSToF, ISEL_BOOLC },
        { SpvOpSConvert,    SpvOpUConvert,    SpvOpConvertUToF, ISEL_BOOLC },
        { SpvOpConvertFToS, SpvOpConvertFToU, SpvOpFConvert,    ISEL_ILLEG },
        { ISEL_BOOLC,       ISEL_BOOLC,       ISEL_ILLEG,       ISEL_IDENT }
    }},

    [reinterpret_op] = { FirstAndResult, TyOperand, .foar = {
        { ISEL_IDENT,         SpvOpBitcast,       SpvOpBitcast, ISEL_ILLEG, SpvOpConvertUToPtr },
        { SpvOpBitcast,       ISEL_IDENT,         SpvOpBitcast, ISEL_ILLEG, SpvOpConvertUToPtr },
        { SpvOpBitcast,       SpvOpBitcast,       ISEL_IDENT,   ISEL_ILLEG, ISEL_ILLEG /* no fp-ptr casts */ },
        { ISEL_ILLEG,         ISEL_ILLEG,         ISEL_ILLEG,   ISEL_IDENT, ISEL_ILLEG /* no bool reinterpret */ },
        { SpvOpConvertPtrToU, SpvOpConvertPtrToU, ISEL_ILLEG,   ISEL_ILLEG, ISEL_IDENT }
    }},

    [PRIMOPS_COUNT] = { Custom }
};

static void emit_primop(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, const Node* instr, Nodes variables) {
    PrimOp prim_op = instr->payload.prim_op;
    Nodes args = prim_op.operands;

    struct IselTableEntry entry = isel_table[prim_op.op];
    if (entry.i_sel_mechanism != Custom) {
        LARRAY(SpvId, arr, args.count);
        for (size_t i = 0; i < args.count; i++) {
            if (!args.nodes[i])
                continue;
            else if (is_type(args.nodes[i]))
                arr[i] = emit_type(emitter, args.nodes[i]);
            else
                arr[i] = emit_value(emitter, args.nodes[i], NULL);
        }

        SpvOp opcode;
        if (entry.i_sel_mechanism == FirstOp) {
            enum OperandKind op_class = classify_operand(args.nodes[0]);
            opcode = entry.fo[op_class];
        } else if (entry.i_sel_mechanism == FirstAndResult) {
            enum OperandKind return_t_class = classify_operand(args.nodes[0]);
            enum OperandKind op_class = classify_operand(args.nodes[1]);
            opcode = entry.foar[op_class][return_t_class];
        }

        if (opcode == 0)
            goto custom_path;

        const Type* result_t;
        switch (entry.result_kind) {
            case Same:      result_t = without_qualifier(args.nodes[0]->type); break;
            case Bool:      result_t = bool_type(emitter->arena); break;
            case TyOperand: result_t = args.nodes[0]; break;
            default: error("unhandled result kind");
        }

        if (args.count == 1)
            register_result(emitter, variables.nodes[0], spvb_unop(bb_builder, opcode, emit_type(emitter, result_t), arr[0]));
        else if (args.count == 2)
            register_result(emitter, variables.nodes[0], spvb_binop(bb_builder, opcode, emit_type(emitter, result_t), arr[0], arr[1]));
        else
            error("unhandled isel for argsc > 2");

        return;
    }

    custom_path:
    switch (prim_op.op) {
        case load_op: {
            assert(without_qualifier(args.nodes[0]->type)->tag == PtrType_TAG);
            const Type* elem_type = without_qualifier(args.nodes[0]->type)->payload.ptr_type.pointed_type;
            SpvId eptr = emit_value(emitter, args.nodes[0], NULL);
            SpvId result = spvb_load(bb_builder, emit_type(emitter, elem_type), eptr, 0, NULL);
            register_result(emitter, variables.nodes[0], result);
            return;
        }
        case store_op: {
            assert(without_qualifier(args.nodes[0]->type)->tag == PtrType_TAG);
            SpvId eptr = emit_value(emitter, args.nodes[0], NULL);
            SpvId eval = emit_value(emitter, args.nodes[1], NULL);
            spvb_store(bb_builder, eval, eptr, 0, NULL);
            return;
        }
        case alloca_op: {
            const Type* elem_type = args.nodes[0];
            SpvId result = spvb_local_variable(fn_builder, emit_type(emitter, ptr_type(emitter->arena, (PtrType) {
                .address_space = AsFunctionLogical,
                .pointed_type = elem_type
            })), SpvStorageClassFunction);
            register_result(emitter, variables.nodes[0], result);
            return;
        }
        case lea_op: {
            SpvId base = emit_value(emitter, args.nodes[0], NULL);

            LARRAY(SpvId, indices, args.count - 2);
            for (size_t i = 2; i < args.count; i++)
                indices[i - 2] = args.nodes[i] ? emit_value(emitter, args.nodes[i], NULL) : 0;

            if (args.nodes[1]) {
                error("TODO: OpPtrAccessChain")
            } else {
                const Type* target_type = instr->type;
                SpvId result = spvb_access_chain(bb_builder, emit_type(emitter, target_type), base, args.count - 2, indices);
                register_result(emitter, variables.nodes[0], result);
            }
            return;
        }
        case select_op: {
            SpvId cond = emit_value(emitter, args.nodes[0], NULL);
            SpvId truv = emit_value(emitter, args.nodes[1], NULL);
            SpvId flsv = emit_value(emitter, args.nodes[2], NULL);

            SpvId result = spvb_select(bb_builder, emit_type(emitter, variables.nodes[0]->type), cond, truv, flsv);
            register_result(emitter, variables.nodes[0], result);
            return;
        }
        default: error("TODO: unhandled op");
    }
    SHADY_UNREACHABLE;
}

static SpvId nodes_to_codom(Emitter* emitter, Nodes return_types) {
    switch (return_types.count) {
        case 0: return emitter->void_t;
        case 1: return emit_type(emitter, return_types.nodes[0]);
        default: {
            const Type* codom_ret_type = record_type(emitter->arena, (RecordType) {.members = return_types});
            return emit_type(emitter, codom_ret_type);
        }
    }
}

static void emit_call(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, Call call, Nodes variables) {
    const Type* callee_type = without_qualifier(call.callee->type);
    assert(callee_type->tag == FnType_TAG);
    SpvId return_type = nodes_to_codom(emitter, callee_type->payload.fn_type.return_types);
    SpvId callee = emit_value(emitter, call.callee, NULL);
    LARRAY(SpvId, args, call.args.count);
    for (size_t i = 0; i < call.args.count; i++)
        args[i] = emit_value(emitter, call.args.nodes[i], NULL);
    SpvId result = spvb_call(bb_builder, return_type, callee, call.args.count, args);
    switch (variables.count) {
        case 0: break;
        case 1: {
            register_result(emitter, variables.nodes[0], result);
            break;
        }
        default: {
            for (size_t i = 0; i < variables.count; i++) {
                SpvId result_type = emit_type(emitter, variables.nodes[i]->type);
                SpvId extracted_component = spvb_extract(bb_builder, result_type, result, 1, (uint32_t []) { i });
                register_result(emitter, variables.nodes[i], extracted_component);
            }
            break;
        }
    }
}

static void emit_if(Emitter* emitter, FnBuilder fn_builder, BBBuilder* bb_builder, MergeTargets* merge_targets, If if_instr, Nodes variables) {
    assert(if_instr.yield_types.count == 0 && "TODO use phis");

    SpvId next_id = spvb_fresh_id(emitter->file_builder);

    SpvId true_id = spvb_fresh_id(emitter->file_builder);
    SpvId false_id = if_instr.if_false ? spvb_fresh_id(emitter->file_builder) : next_id;

    spvb_selection_merge(*bb_builder, next_id, 0);
    SpvId condition = emit_value(emitter, if_instr.condition, NULL);
    spvb_branch_conditional(*bb_builder, condition, true_id, false_id);

    MergeTargets merge_targets_branches = *merge_targets;
    merge_targets_branches.join_target = next_id;

    emit_block(emitter, fn_builder, spvb_begin_bb(fn_builder, true_id), merge_targets_branches, if_instr.if_true);
    if (if_instr.if_false)
        emit_block(emitter, fn_builder, spvb_begin_bb(fn_builder, false_id), merge_targets_branches, if_instr.if_false);

    BBBuilder next = spvb_begin_bb(fn_builder, next_id);
    *bb_builder = next;
}

static void emit_match(Emitter* emitter, FnBuilder fn_builder, BBBuilder* bb_builder, MergeTargets* merge_targets, Match match, Nodes variables) {
    assert(match.yield_types.count == 0 && "TODO use phis");

    SpvId next_id = spvb_fresh_id(emitter->file_builder);

    SpvId default_id = spvb_fresh_id(emitter->file_builder);
    LARRAY(SpvId, literals_and_cases, match.cases.count * 2);
    for (size_t i = 0; i < match.cases.count; i++) {
        literals_and_cases[i * 2 + 0] = spvb_fresh_id(emitter->file_builder);
        literals_and_cases[i * 2 + 1] = spvb_fresh_id(emitter->file_builder);
        emit_value(emitter, match.literals.nodes[i], &literals_and_cases[i * 2 + 0]);
    }

    spvb_selection_merge(*bb_builder, next_id, 0);
    SpvId inspectee = emit_value(emitter, match.inspect, NULL);
    spvb_switch(*bb_builder, inspectee, default_id, match.cases.count, literals_and_cases);

    MergeTargets merge_targets_branches = *merge_targets;
    merge_targets_branches.join_target = next_id;

    for (size_t i = 0; i < match.cases.count; i++) {
        emit_block(emitter, fn_builder, spvb_begin_bb(fn_builder, literals_and_cases[i * 2 + 1]), merge_targets_branches, match.cases.nodes[i]);
    }
    emit_block(emitter, fn_builder, spvb_begin_bb(fn_builder, default_id), merge_targets_branches, match.default_case);

    BBBuilder next = spvb_begin_bb(fn_builder, next_id);
    *bb_builder = next;
}

static void emit_loop(Emitter* emitter, FnBuilder fn_builder, BBBuilder* bb_builder, MergeTargets* merge_targets, Loop loop_instr, Nodes variables) {
    assert(loop_instr.yield_types.count == 0 && "TODO use phis");
    assert(loop_instr.params.count == 0 && "TODO use phis");

    SpvId header_id = spvb_fresh_id(emitter->file_builder);
    SpvId body_id = spvb_fresh_id(emitter->file_builder);
    SpvId continue_id = spvb_fresh_id(emitter->file_builder);
    SpvId next_id = spvb_fresh_id(emitter->file_builder);

    // The current block goes to the header (it can't be the header itself !)
    spvb_branch(*bb_builder, header_id);

    // the header block receives the annotation
    BBBuilder header_builder = spvb_begin_bb(fn_builder, header_id);
    spvb_loop_merge(header_builder, next_id, continue_id, 0, 0, NULL);
    spvb_branch(header_builder, body_id);
    spvb_name(emitter->file_builder, header_id, "loop_header");

    // Emission of the body requires extra info for the break/continue merge terminators
    MergeTargets merge_targets_branches = *merge_targets;
    merge_targets_branches.continue_target = continue_id;
    merge_targets_branches.break_target = next_id;
    BBBuilder body_builder = spvb_begin_bb(fn_builder, body_id);
    emit_block(emitter, fn_builder, body_builder, merge_targets_branches, loop_instr.body);
    spvb_name(emitter->file_builder, body_id, "loop_body");

    // the continue block just jumps back into the header
    BBBuilder continue_builder = spvb_begin_bb(fn_builder, continue_id);
    spvb_branch(continue_builder, header_id);
    spvb_name(emitter->file_builder, continue_id, "loop_continue");

    // We start the next block
    BBBuilder next = spvb_begin_bb(fn_builder, next_id);
    spvb_name(emitter->file_builder, next_id, "loop_next");
    *bb_builder = next;
}

static void emit_instruction(Emitter* emitter, FnBuilder fn_builder, BBBuilder* bb_builder, MergeTargets* merge_targets, const Node* instruction) {
    assert(is_instruction(instruction));
    Nodes variables = nodes(emitter->arena, 0, NULL);

    if (instruction->tag == Let_TAG) {
        variables = instruction->payload.let.variables;
        instruction = instruction->payload.let.instruction;
        assert(is_instruction(instruction) && instruction->tag != Let_TAG);
    }

    switch (instruction->tag) {
        case PrimOp_TAG: emit_primop(emitter, fn_builder, *bb_builder, instruction, variables);                   break;
        case Call_TAG:     emit_call(emitter, fn_builder, *bb_builder, instruction->payload.call_instr, variables);                break;
        case If_TAG:         emit_if(emitter, fn_builder, bb_builder, merge_targets, instruction->payload.if_instr, variables);    break;
        case Match_TAG:   emit_match(emitter, fn_builder, bb_builder, merge_targets, instruction->payload.match_instr, variables); break;
        case Loop_TAG:     emit_loop(emitter, fn_builder, bb_builder, merge_targets, instruction->payload.loop_instr, variables);  break;
        default: error("Unrecognised instruction %s", node_tags[instruction->tag]);
    }
}

static SpvId find_reserved_id(Emitter* emitter, const Node* node) {
    SpvId* found = find_value_dict(const Node*, SpvId, emitter->node_ids, node);
    assert(found);
    return *found;
}

void emit_terminator(Emitter* emitter, FnBuilder fn_builder, BBBuilder basic_block_builder, MergeTargets merge_targets, const Node* terminator) {
    switch (terminator->tag) {
        case Return_TAG: {
            const Nodes* ret_values = &terminator->payload.fn_ret.values;
            switch (ret_values->count) {
                case 0: spvb_return_void(basic_block_builder); return;
                case 1: spvb_return_value(basic_block_builder, emit_value(emitter, ret_values->nodes[0], NULL)); return;
                default: {
                    LARRAY(SpvId, arr, ret_values->count);
                    for (size_t i = 0; i < ret_values->count; i++)
                        arr[i] = emit_value(emitter, ret_values->nodes[i], NULL);
                    SpvId return_that = spvb_composite(basic_block_builder, fn_ret_type_id(fn_builder), ret_values->count, arr);
                    spvb_return_value(basic_block_builder, return_that);
                    return;
                }
            }
        }
        case Branch_TAG: {
            assert(terminator->payload.branch.args.count == 0 && "TODO: implement bb params");
            assert(terminator->payload.branch.yield == false && "Yielding needs to be lowered away");
            switch (terminator->payload.branch.branch_mode) {
                case BrJump: {
                    spvb_branch(basic_block_builder, find_reserved_id(emitter, terminator->payload.branch.target));
                    return;
                }
                case BrIfElse: {
                    SpvId condition = emit_value(emitter, terminator->payload.branch.branch_condition, NULL);
                    spvb_branch_conditional(basic_block_builder, condition, find_reserved_id(emitter, terminator->payload.branch.true_target), find_reserved_id(emitter, terminator->payload.branch.false_target));
                    return;
                }
                case BrSwitch: error("TODO");
                case BrTailcall: error("Lower me beforehand !")
            }
        }
        case Join_TAG: error("Lower me");
        case MergeConstruct_TAG: {
            switch (terminator->payload.merge_construct.construct) {
                case Selection: {
                    assert(terminator->payload.merge_construct.args.count == 0);
                    spvb_branch(basic_block_builder, merge_targets.join_target);
                    return;
                }
                case Continue: {
                    assert(terminator->payload.merge_construct.args.count == 0);
                    spvb_branch(basic_block_builder, merge_targets.continue_target);
                    return;
                }
                case Break: {
                    assert(terminator->payload.merge_construct.args.count == 0);
                    spvb_branch(basic_block_builder, merge_targets.break_target);
                    return;
                }
                default: error("Not a merge.")
            }
        }
        case Unreachable_TAG: {
            spvb_unreachable(basic_block_builder);
            return;
        }
        default: error("TODO: emit terminator %s", node_tags[terminator->tag]);
    }
    SHADY_UNREACHABLE;
}

static void emit_block(Emitter* emitter, FnBuilder fn_builder, BBBuilder basic_block_builder, MergeTargets merge_targets, const Node* node) {
    assert(node->tag == Block_TAG);
    const Block* block = &node->payload.block;
    for (size_t i = 0; i < block->instructions.count; i++)
        emit_instruction(emitter, fn_builder, &basic_block_builder, &merge_targets, block->instructions.nodes[i]);
    emit_terminator(emitter, fn_builder, basic_block_builder, merge_targets, block->terminator);
}

static void emit_basic_block(Emitter* emitter, FnBuilder fn_builder, const CFNode* node, bool is_entry) {
    assert(node->node->tag == Function_TAG);
    // Find the preassigned ID to this
    SpvId bb_id = is_entry ? spvb_fresh_id(emitter->file_builder) : find_reserved_id(emitter, node->node);
    BBBuilder basic_block_builder = spvb_begin_bb(fn_builder, bb_id);
    spvb_name(emitter->file_builder, bb_id, node->node->payload.fn.name);

    MergeTargets merge_targets = {
        .continue_target = 0,
        .break_target = 0,
        .join_target = 0
    };
    emit_block(emitter, fn_builder, basic_block_builder, merge_targets, node->node->payload.fn.block);

    // Emit the child nodes for real
    size_t dom_count = entries_count_list(node->dominates);
    for (size_t i = 0; i < dom_count; i++) {
        CFNode* child_node = read_list(CFNode*, node->dominates)[i];
        emit_basic_block(emitter, fn_builder, child_node, false);
    }
}

static void emit_function(Emitter* emitter, const Node* node) {
    assert(node->tag == Function_TAG);

    const Type* fn_type = node->type;
    FnBuilder fn_builder = spvb_begin_fn(emitter->file_builder, find_reserved_id(emitter, node), emit_type(emitter, fn_type), nodes_to_codom(emitter, node->payload.fn.return_types));

    Nodes params = node->payload.fn.params;
    for (size_t i = 0; i < params.count; i++) {
        SpvId param_id = spvb_parameter(fn_builder, emit_type(emitter, params.nodes[i]->payload.var.type));
        insert_dict_and_get_result(struct Node*, SpvId, emitter->node_ids, params.nodes[i], param_id);
    }

    Scope scope = build_scope(node);
    emit_basic_block(emitter, fn_builder, scope.entry, true);
    dispose_scope(&scope);

    spvb_define_function(emitter->file_builder, fn_builder);
}

static SpvId emit_value(Emitter* emitter, const Node* node, const SpvId* use_id) {
    if (!use_id) { // re-emit the thing multiple times if we need a specific ID
        SpvId* existing = find_value_dict(struct Node*, SpvId, emitter->node_ids, node);
        if (existing)
            return *existing;
    }

    SpvId new = use_id ? *use_id : spvb_fresh_id(emitter->file_builder);
    insert_dict_and_get_result(struct Node*, SpvId, emitter->node_ids, node, new);

    switch (node->tag) {
        case Variable_TAG: error("this node should have been resolved already");
        case IntLiteral_TAG: {
            SpvId ty = emit_type(emitter, node->type);
            // 64-bit constants take two spirv words, anythinfg else fits in one
            if (node->payload.int_literal.width == IntTy64) {
                uint32_t arr[] = { node->payload.int_literal.value_i64 >> 32, node->payload.int_literal.value_i64 & 0xFFFFFFFF };
                spvb_constant(emitter->file_builder, new, ty, 2, arr);
            } else {
                uint32_t arr[] = { node->payload.int_literal.value_i32 };
                spvb_constant(emitter->file_builder, new, ty, 1, arr);
            }
            break;
        }
        case True_TAG: {
            spvb_bool_constant(emitter->file_builder, new, emit_type(emitter, bool_type(emitter->arena)), true);
            break;
        }
        case False_TAG: {
            spvb_bool_constant(emitter->file_builder, new, emit_type(emitter, bool_type(emitter->arena)), false);
            break;
        }
        default: error("don't know hot to emit value");
    }
    return new;
}

static SpvId emit_type(Emitter* emitter, const Type* type) {
    SpvId* existing = find_value_dict(struct Node*, SpvId, emitter->node_ids, type);
    if (existing)
        return *existing;
    
    SpvId new;
    switch (type->tag) {
        case Int_TAG: {
            int width;
            switch (type->payload.int_type.width) {
                case IntTy8:  width = 8;  break;
                case IntTy16: width = 16; break;
                case IntTy32: width = 32; break;
                case IntTy64: width = 64; break;
                default: assert(false);
            }
            new = spvb_int_type(emitter->file_builder, width, true);
            break;
        } case Bool_TAG:
            new = spvb_bool_type(emitter->file_builder);
            break;
        case PtrType_TAG: {
            SpvId pointee = emit_type(emitter, type->payload.ptr_type.pointed_type);
            SpvStorageClass sc = emit_addr_space(type->payload.ptr_type.address_space);
            new = spvb_ptr_type(emitter->file_builder, sc, pointee);
            break;
        }
        case RecordType_TAG: {
            LARRAY(SpvId, members, type->payload.record_type.members.count);
            for (size_t i = 0; i < type->payload.record_type.members.count; i++)
                members[i] = emit_type(emitter, type->payload.record_type.members.nodes[i]);
            new = spvb_struct_type(emitter->file_builder, type->payload.record_type.members.count, members);
            break;
        }
        case FnType_TAG: {
            const FnType* fnt = &type->payload.fn_type;
            assert(!fnt->is_continuation);
            LARRAY(SpvId, params, fnt->param_types.count);
            for (size_t i = 0; i < fnt->param_types.count; i++)
                params[i] = emit_type(emitter, fnt->param_types.nodes[i]);

            new = spvb_fn_type(emitter->file_builder, fnt->param_types.count, params, nodes_to_codom(emitter, fnt->return_types));
            break;
        }
        case QualifiedType_TAG: {
            // SPIR-V does not care about our type qualifiers.
            new = emit_type(emitter, type->payload.qualified_type.type);
            break;
        }
        case ArrType_TAG: {
            SpvId element_type = emit_type(emitter, type->payload.arr_type.element_type);
            if (type->payload.arr_type.size) {
                new = spvb_array_type(emitter->file_builder, element_type, emit_value(emitter, type->payload.arr_type.size, NULL));
            } else {
                new = spvb_runtime_array_type(emitter->file_builder, element_type);
            }
            break;
        }
        default: error("Don't know how to emit type")
    }

    insert_dict_and_get_result(struct Node*, SpvId, emitter->node_ids, type, new);
    return new;
}

void emit_spirv(CompilerConfig* config, IrArena* arena, const Node* root_node, FILE* output) {
    const Root* top_level = &root_node->payload.root;
    struct List* words = new_list(uint32_t);

    FileBuilder file_builder = spvb_begin();

    Emitter emitter = {
        .configuration = config,
        .arena = arena,
        .file_builder = file_builder,
        .node_ids = new_dict(Node*, SpvId, (HashFn) hash_node, (CmpFn) compare_node),
    };

    emitter.void_t = spvb_void_type(emitter.file_builder);

    spvb_capability(file_builder, SpvCapabilityShader);
    spvb_capability(file_builder, SpvCapabilityLinkage);
    spvb_capability(file_builder, SpvCapabilityPhysicalStorageBufferAddresses);
    spvb_capability(file_builder, SpvCapabilitySubgroupBallotKHR);

    // First reserve IDs for declarations
    LARRAY(SpvId, ids, top_level->declarations.count);
    int global_fn_case_number = 1;
    for (size_t i = 0; i < top_level->declarations.count; i++) {
        const Node* decl = top_level->declarations.nodes[i];
        ids[i] = spvb_fresh_id(file_builder);
        insert_dict_and_get_result(struct Node*, SpvId, emitter.node_ids, decl, ids[i]);
    }

    for (size_t i = 0; i < top_level->declarations.count; i++) {
        const Node* decl = top_level->declarations.nodes[i];
        switch (decl->tag) {
            case GlobalVariable_TAG: {
                const GlobalVariable* gvar = &decl->payload.global_variable;
                SpvId init = 0;
                if (gvar->init)
                    init = emit_value(&emitter, gvar->init, NULL);
                spvb_global_variable(file_builder, ids[i], emit_type(&emitter, decl->type), emit_addr_space(gvar->address_space), false, init);
                spvb_name(file_builder, ids[i], gvar->name);
                break;
            } case Function_TAG: {
                emit_function(&emitter, decl);
                spvb_name(file_builder, ids[i], decl->payload.fn.name);
                break;
            } case Constant_TAG: {
                const Constant* cnst = &decl->payload.constant;
                emit_value(&emitter, cnst->value, &ids[i]);
                spvb_name(file_builder, ids[i], cnst->name);
                break;
            }
            default: error("unhandled declaration kind")
        }
    }

    spvb_finish(file_builder, words);

    // cleanup the emitter
    destroy_dict(emitter.node_ids);

    fwrite(words->alloc, words->elements_count, 4, output);

    destroy_list(words);
}
