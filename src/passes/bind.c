#include "passes.h"

#include "list.h"

#include "../implem.h"

#include <assert.h>
#include <string.h>

struct BindEntry {
    const char* id;
    // const struct Node* old_node;
    const struct Node* new_node;
};

struct BindRewriter {
    struct Rewriter rewriter;
    struct List* bound_variables;
};

static const struct Node* resolve(struct BindRewriter* ctx, const char* id) {
    for (size_t i = 0; i < entries_count_list(ctx->bound_variables); i++) {
        const struct BindEntry* entry = &read_list(const struct BindEntry, ctx->bound_variables)[i];
        if (strcmp(entry->id, id) == 0) {
            return entry->new_node;
        }
    }
    error("could not resolve variable %s", id)
}

const struct Node* bind_node(struct BindRewriter* ctx, const struct Node* node) {
    if (node == NULL)
        return NULL;

    struct Rewriter* rewriter = &ctx->rewriter;
    switch (node->tag) {
        case Variable_TAG: {
            assert(node->payload.var.type == NULL);
            return resolve(ctx, node->payload.var.name);
        }
        /*case VariableDecl_TAG: {
            const struct Node* new_variable = var(rewriter->dst_arena, (struct Variable) {
                .name = string(rewriter->dst_arena, node->payload.var_decl.variable->payload.var.name),
                .type = rewriter->rewrite_type(rewriter, node->payload.var_decl.variable->payload.var.type)
            });
            struct BindEntry entry = {
                .id = string(ctx->rewriter.dst_arena, node->payload.var_decl.variable->payload.var.name),
                .new_node = new_variable
            };
            append_list(struct BindEntry, ctx->bound_variables, entry);
            return var_decl(ctx->rewriter.dst_arena, (struct VariableDecl){
                .variable = new_variable,
                .address_space = node->payload.var_decl.address_space,
                .init = rewriter->rewrite_node(rewriter, node->payload.var_decl.init)
            });
        }*/
        case Let_TAG: {
            size_t outputs_count = node->payload.let.variables.count;
            const struct Node* noutputs[outputs_count];
            for (size_t p = 0; p < outputs_count; p++) {
                const struct Variable* old_var = &node->payload.let.variables.nodes[p]->payload.var;
                const struct Node* new_binding = var(rewriter->dst_arena, (struct Variable) {
                    .name = string(rewriter->dst_arena, old_var->name),
                    .type = rewriter->rewrite_type(rewriter, old_var->type)
                });
                noutputs[p] = new_binding;
                struct BindEntry entry = {
                    .id = string(ctx->rewriter.dst_arena, old_var->name),
                    .new_node = new_binding
                };
                append_list(struct BindEntry, ctx->bound_variables, entry);
                printf("Bound %s\n", entry.id);
            }

            return let(rewriter->dst_arena, (struct Let) {
                .variables = nodes(rewriter->dst_arena, outputs_count, noutputs),
                .target = rewriter->rewrite_node(rewriter, node->payload.let.target)
            });
        }
        case Function_TAG: {
            size_t old_bound_variables_size = entries_count_list(ctx->bound_variables);

            size_t params_count = node->payload.fn.params.count;
            const struct Node* nparams[params_count];
            for (size_t p = 0; p < params_count; p++) {
                const struct Variable* old_param = &node->payload.fn.params.nodes[p]->payload.var;
                const struct Node* new_param = var(rewriter->dst_arena, (struct Variable) {
                    .name = string(rewriter->dst_arena, old_param->name),
                    .type = rewriter->rewrite_type(rewriter, old_param->type)
                });
                nparams[p] = new_param;
                struct BindEntry entry = {
                    .id = string(ctx->rewriter.dst_arena, old_param->name),
                    .new_node = new_param
                };
                append_list(struct BindEntry, ctx->bound_variables, entry);
                printf("Bound %s\n", entry.id);
            }

            const struct Node* new_fn = fn(rewriter->dst_arena, (struct Function) {
                .return_type = rewriter->rewrite_type(rewriter, node->payload.fn.return_type),
                .instructions = rewrite_nodes(rewriter, node->payload.fn.instructions),
                .params = nodes(rewriter->dst_arena, params_count, nparams),
            });

            while (entries_count_list(ctx->bound_variables) > old_bound_variables_size)
                pop_list(struct BindEntry, ctx->bound_variables);

            return new_fn;
        }
        default: return recreate_node_identity(&ctx->rewriter, node);
    }
}

const struct Program* bind_program(struct IrArena* src_arena, struct IrArena* dst_arena, const struct Program* src_program) {
    const size_t count = src_program->variables.count;

    const struct Node* new_variables[count];
    const struct Node* new_definitions[count];

    struct List* bound_variables = new_list(struct BindEntry);
    struct BindRewriter ctx = {
        .rewriter = {
            .src_arena = src_arena,
            .dst_arena = dst_arena,
            .rewrite_node = (NodeRewriteFn) bind_node,
            .rewrite_type = (TypeRewriteFn) recreate_type_identity
        },
        .bound_variables = bound_variables
    };

    for (size_t i = 0; i < count; i++) {
        const struct Node* variable = src_program->variables.nodes[i];

        const struct Node* new_variable = var(dst_arena, (struct Variable) {
            .name = string(dst_arena, variable->payload.var.name),
            .type = ctx.rewriter.rewrite_type(&ctx.rewriter, variable->payload.var.type)
        });

        struct BindEntry entry = {
            .id = variable->payload.var.name,
            .new_node = new_variable
        };
        append_list(struct BindEntry, bound_variables, entry);
        new_variables[i] = new_variable;
    }

    for (size_t i = 0; i < count; i++) {
        new_definitions[i] = bind_node(&ctx, src_program->definitions.nodes[i]);
    }

    destroy_list(bound_variables);

    return &program(dst_arena, (struct Program) {
        .variables = nodes(dst_arena, count, new_variables),
        .definitions = nodes(dst_arena, count, new_definitions)
    })->payload.program;
}