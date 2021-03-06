#include "shady/ir.h"

#include "../rewrite.h"
#include "../type.h"
#include "../log.h"
#include "../portability.h"

#include "../transform/ir_gen_helpers.h"

#include "list.h"
#include "dict.h"

#include <assert.h>

typedef uint32_t FnPtr;

typedef struct Context_ {
    Rewriter rewriter;
    struct Dict* assigned_fn_ptrs;
    FnPtr next_fn_ptr;

    const Node* god_fn;

    const Node* next_fn_var;
    const Node* next_mask_var;
    const Node* branch_fn;
    const Node* join_fn;
    struct List* new_decls;
} Context;

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

static const Node* fn_ptr_as_value(IrArena* arena, FnPtr ptr) {
    return int_literal(arena, (IntLiteral) {
        .value_i32 = ptr,
        .width = IntTy32
    });
}

static const Node* lower_fn_addr(Context* ctx, const Node* the_function) {
    assert(the_function->tag == Function_TAG);

    FnPtr* found = find_value_dict(const Node*, FnPtr, ctx->assigned_fn_ptrs, the_function);
    if (found) return fn_ptr_as_value(ctx->rewriter.dst_arena, *found);

    FnPtr ptr = ctx->next_fn_ptr++;
    bool r = insert_dict_and_get_result(const Node*, FnPtr, ctx->assigned_fn_ptrs, the_function, ptr);
    assert(r);
    return fn_ptr_as_value(ctx->rewriter.dst_arena, ptr);
}

static const Node* push_args_stack(Context* ctx, Nodes args, BlockBuilder* builder) {
    for (size_t i = 0; i < args.count; i++) {
        assert("TODO");
    }
}

static const Node* rewrite_block(Context* ctx, const Node* old_block, BlockBuilder* block_builder) {
    IrArena* arena = ctx->rewriter.dst_arena;
    Nodes old_instructions = old_block->payload.block.instructions;
    for (size_t i = 0; i < old_instructions.count; i++) {
        const Node* rewritten = rewrite_node(&ctx->rewriter, old_instructions.nodes[i]);
        append_block(block_builder, rewritten);
    }

    const Node* old_terminator = old_block->payload.block.terminator;
    const Node* new_terminator = NULL;

    switch (old_terminator->tag) {
        case Branch_TAG: {
            assert(old_terminator->payload.branch.branch_mode == BrTailcall);
            push_args_stack(ctx, rewrite_nodes(&ctx->rewriter, old_terminator->payload.branch.args), block_builder);

            const Node* target = rewrite_node(&ctx->rewriter, old_terminator->payload.branch.target);

            const Node* call = call_instr(arena, (Call) {
                .callee = ctx->join_fn,
                .args = nodes(arena, 2, (const Node*[]) { target })
            });

            append_block(block_builder, call);

            new_terminator = fn_ret(arena, (Return) { .fn = NULL, .values = nodes(arena, 0, NULL) });
            break;
        }
        case Join_TAG: {
            assert(old_terminator->payload.join.is_indirect);
            push_args_stack(ctx, rewrite_nodes(&ctx->rewriter, old_terminator->payload.join.args), block_builder);

            const Node* target = rewrite_node(&ctx->rewriter, old_terminator->payload.join.join_at);
            const Node* mask = rewrite_node(&ctx->rewriter, old_terminator->payload.join.desired_mask);

            const Node* call = call_instr(arena, (Call) {
                .callee = ctx->join_fn,
                .args = nodes(arena, 2, (const Node*[]) { target, mask })
            });

            append_block(block_builder, call);

            new_terminator = fn_ret(arena, (Return) { .fn = NULL, .values = nodes(arena, 0, NULL) });
            break;
        }

        case Callc_TAG:
        case Return_TAG: error("We expect that stuff to be already lowered here !")

        case Unreachable_TAG:
        case MergeConstruct_TAG: new_terminator = rewrite_node(&ctx->rewriter, old_terminator); break;
        default: error("Unknown terminator");
    }

    assert(new_terminator);

    return finish_block(block_builder, new_terminator);
}

static const Node* lower_callf_process(Context* ctx, const Node* old) {
    IrArena* dst_arena = ctx->rewriter.dst_arena;
    switch (old->tag) {
        case GlobalVariable_TAG:
        case Constant_TAG: {
            Node* new = recreate_decl_header_identity(&ctx->rewriter, old);
            recreate_decl_body_identity(&ctx->rewriter, old, new);
            return new;
        }
        case Function_TAG: {
            FnAttributes nattrs = old->payload.fn.atttributes;
            nattrs.entry_point_type = NotAnEntryPoint;
            String new_name = nattrs.is_continuation ? old->payload.fn.name : format_string(dst_arena, "%s_leaf", old->payload.fn.name);

            Node* fun = fn(dst_arena, nattrs, new_name, nodes(dst_arena, 0, NULL), nodes(dst_arena, 0, NULL));

            if (old->payload.fn.atttributes.entry_point_type != NotAnEntryPoint) {
                Node* new_entry_pt = fn(dst_arena, old->payload.fn.atttributes, old->payload.fn.name, old->payload.fn.params, nodes(dst_arena, 0, NULL));
                append_list(const Node*, ctx->new_decls, new_entry_pt);

                BlockBuilder* builder = begin_block(dst_arena);
                for (size_t i = fun->payload.fn.params.count - 1; i < fun->payload.fn.params.count; i--) {
                    gen_push_value_stack(builder, fun->payload.fn.params.nodes[i]);
                }

                gen_store(builder, ctx->next_fn_var, lower_fn_addr(ctx, fun));
                const Node* entry_mask = gen_primop(builder, (PrimOp) {
                    .op = subgroup_active_mask_op,
                    .operands = nodes(dst_arena, 0, NULL)
                }).nodes[0];
                gen_store(builder, ctx->next_mask_var, entry_mask);

                append_block(builder, call_instr(dst_arena, (Call) {
                    .callee = ctx->god_fn,
                    .args = nodes(dst_arena, 0, NULL)
                }));

                new_entry_pt->payload.fn.block = finish_block(builder, fn_ret(dst_arena, (Return) {
                    .fn = NULL,
                    .values = nodes(dst_arena, 0, NULL)
                }));
            }

            register_processed(&ctx->rewriter, old, fun);
            BlockBuilder* block_builder = begin_block(dst_arena);

            // Params become stack pops !
            for (size_t i = 0; i < fun->payload.fn.params.count; i++) {
                const Node* old_param = old->payload.fn.params.nodes[i];
                const Node* popped = gen_pop_value_stack(block_builder, format_string(dst_arena, "arg%d", i), rewrite_node(&ctx->rewriter, without_qualifier(old_param->type)));
                register_processed(&ctx->rewriter, old_param, popped);
            }
            fun->payload.fn.block = rewrite_block(ctx, old->payload.fn.block, block_builder);

            return fun;
        }
        case FnAddr_TAG: return lower_fn_addr(ctx, old->payload.fn_addr.fn);
        case Block_TAG: {
            BlockBuilder* builder = begin_block(ctx->rewriter.dst_arena);
            return rewrite_block(ctx, old, builder);
        }
        case PtrType_TAG: {
            const Node* pointee = old->payload.ptr_type.pointed_type;
            if (pointee->tag == FnType_TAG) {
                const Type* emulated_fn_ptr_type = int32_type(ctx->rewriter.dst_arena);
                return emulated_fn_ptr_type;
            }
            // fallthrough
        }
        default: return recreate_node_identity(&ctx->rewriter, old);
    }
}

void generate_top_level_dispatch_fn(Context* ctx, const Node* old_root, Node* dispatcher_fn) {
    IrArena* dst_arena = ctx->rewriter.dst_arena;

    BlockBuilder* loop_body_builder = begin_block(dst_arena);

    const Node* next_function = gen_load(loop_body_builder, ctx->next_fn_var);

    struct List* literals = new_list(const Node*);
    struct List* cases = new_list(const Node*);

    const Node* zero_lit = int_literal(dst_arena, (IntLiteral) { .value_i32 = 0, .width = IntTy32 });
    const Node* zero_case = block(dst_arena, (Block) {
        .instructions = nodes(dst_arena, 0, NULL),
        .terminator = merge_construct(dst_arena, (MergeConstruct) {
            .args = nodes(dst_arena, 0, NULL),
            .construct = Break
        })
    });

    append_list(const Node*, literals, zero_lit);
    append_list(const Node*, cases, zero_case);

    for (size_t i = 0; i < old_root->payload.root.declarations.count; i++) {
        const Node* decl = old_root->payload.root.declarations.nodes[i];
        if (decl->tag == Function_TAG) {
            const Node* fn_lit = lower_fn_addr(ctx, find_processed(&ctx->rewriter, decl));

            const FnType* fn_type = &without_qualifier(decl->type)->payload.fn_type;
            BlockBuilder* case_builder = begin_block(dst_arena);

            // LARRAY(const Node*, fn_args, fn_type->param_types.count);
            // for (size_t j = 0; j < fn_type->param_types.count; j++) {
            //     fn_args[j] = gen_pop_value_stack(case_builder, format_string(dst_arena, "arg_%d", (int) j), without_qualifier(fn_type->param_types.nodes[j]));
            // }

            // TODO wrap in if(mask)
            append_block(case_builder, call_instr(dst_arena, (Call) {
                .callee = find_processed(&ctx->rewriter, decl),
                // .args = nodes(dst_arena, fn_type->param_types.count, fn_args)
                .args = nodes(dst_arena, 0, NULL)
            }));

            const Node* fn_case = finish_block(case_builder, merge_construct(dst_arena, (MergeConstruct) {
                .args = nodes(dst_arena, 0, NULL),
                .construct = Continue
            }));

            append_list(const Node*, literals, fn_lit);
            append_list(const Node*, cases, fn_case);
        }
    }

    append_block(loop_body_builder, match_instr(dst_arena, (Match) {
        .yield_types = nodes(dst_arena, 0, NULL),
        .inspect = next_function,
        .literals = nodes(dst_arena, entries_count_list(literals), read_list(const Node*, literals)),
        .cases = nodes(dst_arena, entries_count_list(cases), read_list(const Node*, cases)),
        .default_case = block(dst_arena, (Block) {
            .instructions = nodes(dst_arena, 0, NULL),
            .terminator = unreachable(dst_arena)
        })
    }));

    destroy_list(literals);
    destroy_list(cases);

    const Node* loop_body = finish_block(loop_body_builder, unreachable(dst_arena));

    Nodes dispatcher_body_instructions = nodes(dst_arena, 1, (const Node* []) { loop_instr(dst_arena, (Loop) {
        .yield_types = nodes(dst_arena, 0, NULL),
        .params = nodes(dst_arena, 0, NULL),
        .initial_args = nodes(dst_arena, 0, NULL),
        .body = loop_body
    }) });

    dispatcher_fn->payload.fn.block = block(dst_arena, (Block) {
        .instructions = dispatcher_body_instructions,
        .terminator = fn_ret(dst_arena, (Return) {
            .values = nodes(dst_arena, 0, NULL),
            .fn = NULL,
        })
    });
}

const Node* lower_callf(SHADY_UNUSED CompilerConfig* config, IrArena* src_arena, IrArena* dst_arena, const Node* src_program) {
    struct List* new_decls_list = new_list(const Node*);
    struct Dict* done = new_dict(const Node*, Node*, (HashFn) hash_node, (CmpFn) compare_node);
    struct Dict* ptrs = new_dict(const Node*, FnPtr, (HashFn) hash_node, (CmpFn) compare_node);

    Node* dispatcher_fn = fn(dst_arena, (FnAttributes) {.entry_point_type = NotAnEntryPoint, .is_continuation = false}, "top_dispatcher", nodes(dst_arena, 0, NULL), nodes(dst_arena, 0, NULL));
    append_list(const Node*, new_decls_list, dispatcher_fn);

    Context ctx = {
        .rewriter = {
            .dst_arena = dst_arena,
            .src_arena = src_arena,
            .rewrite_fn = (RewriteFn) lower_callf_process,
            .rewrite_decl_body = NULL,
            .processed = done,
        },
        .assigned_fn_ptrs = ptrs,
        .next_fn_ptr = 1,

        .new_decls = new_decls_list,
        .god_fn = dispatcher_fn,
    };

    const Node* rewritten = recreate_node_identity(&ctx.rewriter, src_program);

    generate_top_level_dispatch_fn(&ctx, src_program, dispatcher_fn);

    Nodes new_decls = rewritten->payload.root.declarations;
    for (size_t i = 0; i < entries_count_list(new_decls_list); i++) {
        new_decls = append_nodes(dst_arena, new_decls, read_list(const Node*, new_decls_list)[i]);
    }
    rewritten = root(dst_arena, (Root) {
        .declarations = new_decls
    });

    destroy_list(new_decls_list);

    destroy_dict(done);
    destroy_dict(ptrs);
    return rewritten;
}
