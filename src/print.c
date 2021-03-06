#include "shady/ir.h"
#include "analysis/scope.h"

#include "log.h"
#include "list.h"

#include <assert.h>
#include <inttypes.h>

struct PrinterCtx {
    FILE* output;
    unsigned int indent;
    bool print_ptrs;
};

#define printf(...) fprintf(ctx->output, __VA_ARGS__)
#define print_node(n) print_node_impl(ctx, n)

static void print_node_impl(struct PrinterCtx* ctx, const Node* node);

#define INDENT for (unsigned int j = 0; j < ctx->indent; j++) \
    printf("   ");

static void print_storage_qualifier_for_global(struct PrinterCtx* ctx, AddressSpace as) {
    switch (as) {
        case AsGeneric:             printf("generic"); break;

        case AsFunctionLogical:  printf("l_function"); break;
        case AsPrivateLogical:      printf("private"); break;
        case AsSharedLogical:        printf("shared"); break;
        case AsGlobalLogical:        printf("global"); break;

        case AsPrivatePhysical:   printf("p_private"); break;
        case AsSubgroupPhysical: printf("p_subgroup"); break;
        case AsSharedPhysical:     printf("p_shared"); break;
        case AsGlobalPhysical:     printf("p_global"); break;

        case AsInput:                 printf("input"); break;
        case AsOutput:               printf("output"); break;
        case AsExternal:           printf("external"); break;
    }
}

static void print_ptr_addr_space(struct PrinterCtx* ctx, AddressSpace as) {
    switch (as) {
        case AsGeneric:             printf("generic"); break;

        case AsFunctionLogical:  printf("l_function"); break;
        case AsPrivateLogical:    printf("l_private"); break;
        case AsSharedLogical:      printf("l_shared"); break;
        case AsGlobalLogical:      printf("l_global"); break;

        case AsPrivatePhysical:     printf("private"); break;
        case AsSubgroupPhysical:   printf("subgroup"); break;
        case AsSharedPhysical:       printf("shared"); break;
        case AsGlobalPhysical:       printf("global"); break;

        case AsInput:                 printf("input"); break;
        case AsOutput:               printf("output"); break;
        case AsExternal:           printf("external"); break;
        case AsProgramCode:    printf("program_code"); break;
        default: error("Unknown address space: %d", (int) as);
    }
}

static void print_param_list(struct PrinterCtx* ctx, Nodes vars, const Nodes* defaults) {
    if (defaults != NULL)
        assert(defaults->count == vars.count);
    printf("(");
    for (size_t i = 0; i < vars.count; i++) {
        if (ctx->print_ptrs) printf("%zu::", (size_t)(void*)vars.nodes[i]);
        const Variable* var = &vars.nodes[i]->payload.var;
        print_node(var->type);
        printf(" %s_%d", var->name, var->id);
        if (defaults) {
            printf(" = ");
            print_node(defaults->nodes[i]);
        }
        if (i < vars.count - 1)
            printf(", ");
    }
    printf(")");
}

static void print_yield_types(struct PrinterCtx* ctx, Nodes types) {
    bool space = false;
    for (size_t i = 0; i < types.count; i++) {
        if (!space) {
            printf(" ");
            space = true;
        }
        print_node(types.nodes[i]);
        if (i < types.count - 1)
            printf(" ");
        //else
        //    printf("");
    }
}

static void print_function(struct PrinterCtx* ctx, const Node* node) {
    print_yield_types(ctx, node->payload.fn.return_types);
    print_param_list(ctx, node->payload.fn.params, NULL);
    printf(" {\n");
    ctx->indent++;
    print_node(node->payload.fn.block);

    if (node->type != NULL && node->payload.fn.block) {
        bool section_space = false;
        Scope scope = build_scope(node);
        for (size_t i = 1; i < scope.size; i++) {
            if (!section_space) {
                printf("\n");
                section_space = true;
            }

            const CFNode* cfnode = read_list(CFNode*, scope.contents)[i];
            INDENT
            printf("cont %s = ", cfnode->node->payload.fn.name);
            print_param_list(ctx, cfnode->node->payload.fn.params, NULL);
            printf(" {\n");
            ctx->indent++;
            print_node(cfnode->node->payload.fn.block);
            ctx->indent--;
            INDENT
            printf("} \n");
        }
        dispose_scope(&scope);
    }

    ctx->indent--;
    INDENT printf("}");
}

static void print_node_impl(struct PrinterCtx* ctx, const Node* node) {

    if (node == NULL) {
        printf("?");
        return;
    }

    if (ctx->print_ptrs) printf("%zu::", (size_t)(void*)node);

    switch (node->tag) {
        // --------------------------- TYPES
        case QualifiedType_TAG:
            if (node->payload.qualified_type.is_uniform)
                printf("uniform ");
            else
                printf("varying ");
            print_node(node->payload.qualified_type.type);
            break;
        case NoRet_TAG:
            printf("!");
            break;
        case Int_TAG:
            switch (node->payload.int_literal.width) {
                case IntTy8:  printf("i8");  break;
                case IntTy16: printf("i16"); break;
                case IntTy32: printf("i32"); break;
                case IntTy64: printf("i64"); break;
                default: error("Not a known valid int width")
            }
            break;
        case Bool_TAG:
            printf("bool");
            break;
        case Float_TAG:
            printf("float");
            break;
        case MaskType_TAG:
            printf("mask");
            break;
        case RecordType_TAG:
            printf("struct {");
            const Nodes* members = &node->payload.record_type.members;
            for (size_t i = 0; i < members->count; i++) {
                print_node(members->nodes[i]);
                if (i < members->count - 1)
                    printf(", ");
            }
            printf("}");
            break;
        case FnType_TAG: {
            if (node->payload.fn_type.is_continuation)
                printf("cont");
            else {
                printf("fn ");
                const Nodes* returns = &node->payload.fn_type.return_types;
                for (size_t i = 0; i < returns->count; i++) {
                    print_node(returns->nodes[i]);
                    if (i < returns->count - 1)
                        printf(", ");
                }
            }
            printf("(");
            const Nodes* params = &node->payload.fn_type.param_types;
            for (size_t i = 0; i < params->count; i++) {
                print_node(params->nodes[i]);
                if (i < params->count - 1)
                    printf(", ");
            }
            printf(")");
            break;
        }
        case PtrType_TAG: {
            printf("ptr(");
            print_ptr_addr_space(ctx, node->payload.ptr_type.address_space);
            printf(", ");
            print_node(node->payload.ptr_type.pointed_type);
            printf(")");
            break;
        }
        case ArrType_TAG: {
            printf("[");
            print_node(node->payload.arr_type.element_type);
            if (node->payload.arr_type.size) {
                printf("; ");
                print_node(node->payload.arr_type.size);
            }
            printf("]");
            break;
        }

        case Root_TAG: {
            const Root* top_level = &node->payload.root;
            for (size_t i = 0; i < top_level->declarations.count; i++) {
                const Node* decl = top_level->declarations.nodes[i];
                if (ctx->print_ptrs) printf("%zu::", (size_t)(void*)decl);
                if (decl->tag == GlobalVariable_TAG) {
                    const GlobalVariable* gvar = &decl->payload.global_variable;
                    print_storage_qualifier_for_global(ctx, gvar->address_space);
                    printf(" ");
                    print_node(gvar->type);
                    printf(" %s", gvar->name);
                    if (gvar->init) {
                        printf(" = ");
                        print_node(gvar->init);
                    }
                    printf(";\n");
                } else if (decl->tag == Function_TAG) {
                    const Function* fun = &decl->payload.fn;
                    assert(!fun->atttributes.is_continuation);
                    printf("fn");
                    switch (fun->atttributes.entry_point_type) {
                        case Compute: printf(" @compute"); break;
                        case Fragment: printf(" @fragment"); break;
                        case Vertex: printf(" @vertex"); break;
                        default: break;
                    }
                    printf(" %s", fun->name);
                    print_function(ctx, decl);
                    printf(";\n\n");
                } else if (decl->tag == Constant_TAG) {
                    const Constant* cnst = &decl->payload.constant;
                    printf("const ");
                    print_node(decl->type);
                    printf(" %s = ", cnst->name);
                    print_node(cnst->value);
                    printf(";\n");
                } else error("Unammed node at the top level")
            }
            break;
        }
        case Constant_TAG: {
            printf("%s", node->payload.constant.name);
            break;
        }
        case GlobalVariable_TAG: {
            printf("%s", node->payload.global_variable.name);
            break;
        }
        case Variable_TAG:
            printf("%s_%d", node->payload.var.name, node->payload.var.id);
            break;
        case Unbound_TAG:
            printf("`%s`", node->payload.unbound.name);
            break;
        case FnAddr_TAG:
            printf("&");
            print_node(node->payload.fn_addr.fn);
            break;
        case Function_TAG:
            printf("%s", node->payload.fn.name);
            break;
        case Block_TAG: {
            const Block* block = &node->payload.block;
            for(size_t i = 0; i < block->instructions.count; i++) {
                INDENT
                print_node(block->instructions.nodes[i]);
                printf(";\n");
            }
            INDENT
            print_node(block->terminator);
            printf("\n");
            break;
        }
        case ParsedBlock_TAG: {
            const ParsedBlock* pblock = &node->payload.parsed_block;
            for(size_t i = 0; i < pblock->instructions.count; i++) {
                INDENT
                print_node(pblock->instructions.nodes[i]);
                printf(";\n");
            }
            INDENT
            print_node(pblock->terminator);
            printf("\n");

            if (pblock->continuations.count > 0) {
                printf("\n");
            }
            for(size_t i = 0; i < pblock->continuations.count; i++) {
                INDENT
                print_node_impl(ctx, pblock->continuations.nodes[i]);
            }
            break;
        }
        case UntypedNumber_TAG:
            printf("%s", node->payload.untyped_number.plaintext);
            break;
        case IntLiteral_TAG:
            switch (node->payload.int_literal.width) {
                case IntTy8:  printf("%" PRIu8,  node->payload.int_literal.value_i8);  break;
                case IntTy16: printf("%" PRIu16, node->payload.int_literal.value_i16); break;
                case IntTy32: printf("%" PRIu32, node->payload.int_literal.value_i32); break;
                case IntTy64: printf("%" PRIu64, node->payload.int_literal.value_i64); break;
                default: error("Not a known valid int width")
            }
            break;
        case True_TAG:
            printf("true");
            break;
        case False_TAG:
            printf("false");
            break;
        // ----------------- INSTRUCTIONS
        case Let_TAG:
            if (node->payload.let.variables.count > 0) {
                if (node->payload.let.is_mutable)
                    printf("var");
                else
                    printf("let");
                for (size_t i = 0; i < node->payload.let.variables.count; i++) {
                    printf(" ");
                    print_node(node->payload.let.variables.nodes[i]->payload.var.type);
                    printf(" %s", node->payload.let.variables.nodes[i]->payload.var.name);
                    printf("_%d", node->payload.let.variables.nodes[i]->payload.var.id);
                }
                printf(" = ");
            }
            print_node(node->payload.let.instruction);
            break;
        case PrimOp_TAG:
            printf("%s(", primop_names[node->payload.prim_op.op]);
            for (size_t i = 0; i < node->payload.prim_op.operands.count; i++) {
                print_node(node->payload.prim_op.operands.nodes[i]);
                if (i + 1 < node->payload.prim_op.operands.count)
                    printf(", ");
            }
            printf(")");
            break;
        case Call_TAG:
            printf("call ");
            print_node(node->payload.call_instr.callee);
            printf(" ");
            for (size_t i = 0; i < node->payload.call_instr.args.count; i++) {
                printf(" ");
                print_node(node->payload.call_instr.args.nodes[i]);
            }
            break;
        case If_TAG:
            printf("if");
            print_yield_types(ctx, node->payload.if_instr.yield_types);
            printf("(");
            print_node(node->payload.if_instr.condition);
            printf(")");
            printf(" {\n");
            ctx->indent++;
            print_node(node->payload.if_instr.if_true);
            ctx->indent--;
            if (node->payload.if_instr.if_false) {
                INDENT printf("} else {\n");
                ctx->indent++;
                print_node(node->payload.if_instr.if_false);
                ctx->indent--;
            } // else if (node->payload.if_instr.)
            INDENT printf("}");
            break;
        case Loop_TAG:
            printf("loop");
            print_yield_types(ctx, node->payload.loop_instr.yield_types);
            print_param_list(ctx, node->payload.loop_instr.params, &node->payload.loop_instr.initial_args);
            printf(" {\n");
            ctx->indent++;
            print_node(node->payload.loop_instr.body);
            ctx->indent--;
            INDENT printf("}");
            break;
        case Match_TAG:
            printf("match");
            print_yield_types(ctx, node->payload.match_instr.yield_types);
            printf("(");
            print_node(node->payload.match_instr.inspect);
            printf(")");
            printf(" {\n");
            ctx->indent++;
            for (size_t i = 0; i < node->payload.match_instr.literals.count; i++) {
                INDENT
                printf("case ");
                print_node(node->payload.match_instr.literals.nodes[i]);
                printf(": {\n");
                ctx->indent++;
                print_node(node->payload.match_instr.cases.nodes[i]);
                ctx->indent--;
                INDENT printf("}\n");
            }

            INDENT
            printf("default");
            printf(": {\n");
            ctx->indent++;
            print_node(node->payload.match_instr.default_case);
            ctx->indent--;
            INDENT printf("}\n");

            ctx->indent--;
            INDENT printf("}");
            break;
        // --------------------- TERMINATORS
        case Return_TAG:
            printf("return");
            for (size_t i = 0; i < node->payload.fn_ret.values.count; i++) {
                printf(" ");
                print_node(node->payload.fn_ret.values.nodes[i]);
            }
            break;
        case Branch_TAG:
            switch (node->payload.branch.branch_mode) {
                case BrTailcall: printf("tail_call ");   break;
                case BrJump:     printf("jump ");        break;
                case BrIfElse:   printf("br_ifelse ");   break;
                case BrSwitch:   printf("br_switch ");   break;
                default: error("unknown branch mode");
            }
            if (node->payload.branch.yield)
                printf("yield ");
            switch (node->payload.branch.branch_mode) {
                case BrTailcall:
                case BrJump: {
                    print_node(node->payload.branch.target);
                    break;
                }
                case BrIfElse: {
                    printf("(");
                    print_node(node->payload.branch.branch_condition);
                    printf(" ? ");
                    print_node(node->payload.branch.true_target);
                    printf(" : ");
                    print_node(node->payload.branch.false_target);
                    printf(")");
                    break;
                }
                case BrSwitch: {
                    print_node(node->payload.branch.switch_value);
                    printf(" ? (");
                    for (size_t i = 0; i < node->payload.branch.case_values.count; i++) {
                        print_node(node->payload.branch.case_values.nodes[i]);
                        printf(" ");
                        print_node(node->payload.branch.case_targets.nodes[i]);
                        if (i + 1 < node->payload.branch.case_values.count)
                            printf(", ");
                    }
                    printf(" : ");
                    print_node(node->payload.branch.default_target);
                    printf(") ");
                }
            }
            for (size_t i = 0; i < node->payload.branch.args.count; i++) {
                printf(" ");
                print_node(node->payload.branch.args.nodes[i]);
            }
            break;
        case Join_TAG:
            if (node->payload.join.is_indirect)
                printf("joinf ");
            else
                printf("joinc ");
            print_node(node->payload.join.join_at);
            printf(" ");
            print_node(node->payload.join.desired_mask);
            for (size_t i = 0; i < node->payload.join.args.count; i++) {
                printf(" ");
                print_node(node->payload.join.args.nodes[i]);
            }
            break;
        case Callc_TAG:
            if (node->payload.callc.is_return_indirect)
                printf("callf ");
            else
                printf("callc ");
            print_node(node->payload.callc.ret_cont);
            printf(" ");
            print_node(node->payload.callc.callee);
            for (size_t i = 0; i < node->payload.callc.args.count; i++) {
                printf(" ");
                print_node(node->payload.callc.args.nodes[i]);
            }
            break;
        case Unreachable_TAG:
            printf("unreachable ");
            break;
        case MergeConstruct_TAG:
            printf("%s ", merge_what_string[node->payload.merge_construct.construct]);
            for (size_t i = 0; i < node->payload.merge_construct.args.count; i++) {
                print_node(node->payload.merge_construct.args.nodes[i]);
                printf(" ");
            }
            break;
        default: error("dunno how to print %s", node_tags[node->tag]);
    }
}

#undef print_node
#undef printf

static void print_node_in_output(FILE* output, const Node* node, bool dump_ptrs) {
    struct PrinterCtx ctx = {
        .output = output,
        .indent = 0,
        .print_ptrs = dump_ptrs
    };
    print_node_impl(&ctx, node);
}

void print_node(const Node* node) {
    print_node_in_output(stdout, node, false);
}

void log_node(LogLevel level, const Node* node) {
    if (level >= log_level)
        print_node_in_output(stderr, node, false);
}

void dump_node(const Node* node) {
    print_node(node);
    printf("\n");
}
