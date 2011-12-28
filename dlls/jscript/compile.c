/*
 * Copyright 2011 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <math.h>
#include <assert.h>

#include "jscript.h"
#include "engine.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(jscript);

typedef struct _statement_ctx_t {
    unsigned stack_use;
    BOOL using_scope;
    BOOL using_except;

    unsigned break_label;
    unsigned continue_label;

    struct _statement_ctx_t *next;
} statement_ctx_t;

struct _compiler_ctx_t {
    parser_ctx_t *parser;
    bytecode_t *code;

    unsigned code_off;
    unsigned code_size;

    unsigned *labels;
    unsigned labels_size;
    unsigned labels_cnt;

    statement_ctx_t *stat_ctx;

    BOOL no_fallback;
};

static const struct {
    const char *op_str;
    instr_arg_type_t arg1_type;
    instr_arg_type_t arg2_type;
} instr_info[] = {
#define X(n,a,b,c) {#n,b,c},
OP_LIST
#undef X
};

static HRESULT compile_expression(compiler_ctx_t*,expression_t*);
static HRESULT compile_statement(compiler_ctx_t*,statement_ctx_t*,statement_t*);

static inline void *compiler_alloc(bytecode_t *code, size_t size)
{
    return jsheap_alloc(&code->heap, size);
}

static WCHAR *compiler_alloc_string(bytecode_t *code, const WCHAR *str)
{
    size_t size;
    WCHAR *ret;

    size = (strlenW(str)+1)*sizeof(WCHAR);
    ret = compiler_alloc(code, size);
    if(ret)
        memcpy(ret, str, size);
    return ret;
}

static BSTR compiler_alloc_bstr(compiler_ctx_t *ctx, const WCHAR *str)
{
    if(!ctx->code->bstr_pool_size) {
        ctx->code->bstr_pool = heap_alloc(8 * sizeof(BSTR));
        if(!ctx->code->bstr_pool)
            return NULL;
        ctx->code->bstr_pool_size = 8;
    }else if(ctx->code->bstr_pool_size == ctx->code->bstr_cnt) {
        BSTR *new_pool;

        new_pool = heap_realloc(ctx->code->bstr_pool, ctx->code->bstr_pool_size*2*sizeof(BSTR));
        if(!new_pool)
            return NULL;

        ctx->code->bstr_pool = new_pool;
        ctx->code->bstr_pool_size *= 2;
    }

    ctx->code->bstr_pool[ctx->code->bstr_cnt] = SysAllocString(str);
    if(!ctx->code->bstr_pool[ctx->code->bstr_cnt])
        return NULL;

    return ctx->code->bstr_pool[ctx->code->bstr_cnt++];
}

static unsigned push_instr(compiler_ctx_t *ctx, jsop_t op)
{
    assert(ctx->code_size >= ctx->code_off);

    if(!ctx->code_size) {
        ctx->code->instrs = heap_alloc(64 * sizeof(instr_t));
        if(!ctx->code->instrs)
            return -1;
        ctx->code_size = 64;
    }else if(ctx->code_size == ctx->code_off) {
        instr_t *new_instrs;

        new_instrs = heap_realloc(ctx->code->instrs, ctx->code_size*2*sizeof(instr_t));
        if(!new_instrs)
            return -1;

        ctx->code->instrs = new_instrs;
        ctx->code_size *= 2;
    }

    ctx->code->instrs[ctx->code_off].op = op;
    return ctx->code_off++;
}

static inline instr_t *instr_ptr(compiler_ctx_t *ctx, unsigned off)
{
    assert(off < ctx->code_off);
    return ctx->code->instrs + off;
}

static HRESULT push_instr_int(compiler_ctx_t *ctx, jsop_t op, LONG arg)
{
    unsigned instr;

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.lng = arg;
    return S_OK;
}

static HRESULT push_instr_str(compiler_ctx_t *ctx, jsop_t op, const WCHAR *arg)
{
    unsigned instr;
    WCHAR *str;

    str = compiler_alloc_string(ctx->code, arg);
    if(!str)
        return E_OUTOFMEMORY;

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.str = str;
    return S_OK;
}

static HRESULT push_instr_bstr(compiler_ctx_t *ctx, jsop_t op, const WCHAR *arg)
{
    unsigned instr;
    WCHAR *str;

    str = compiler_alloc_bstr(ctx, arg);
    if(!str)
        return E_OUTOFMEMORY;

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.bstr = str;
    return S_OK;
}

static HRESULT push_instr_bstr_uint(compiler_ctx_t *ctx, jsop_t op, const WCHAR *arg1, unsigned arg2)
{
    unsigned instr;
    WCHAR *str;

    str = compiler_alloc_bstr(ctx, arg1);
    if(!str)
        return E_OUTOFMEMORY;

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.bstr = str;
    instr_ptr(ctx, instr)->arg2.uint = arg2;
    return S_OK;
}

static HRESULT push_instr_uint_str(compiler_ctx_t *ctx, jsop_t op, unsigned arg1, const WCHAR *arg2)
{
    unsigned instr;
    WCHAR *str;

    str = compiler_alloc_string(ctx->code, arg2);
    if(!str)
        return E_OUTOFMEMORY;

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.uint = arg1;
    instr_ptr(ctx, instr)->arg2.str = str;
    return S_OK;
}

static HRESULT push_instr_double(compiler_ctx_t *ctx, jsop_t op, double arg)
{
    unsigned instr;
    DOUBLE *dbl;

    dbl = compiler_alloc(ctx->code, sizeof(arg));
    if(!dbl)
        return E_OUTOFMEMORY;
    *dbl = arg;

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.dbl = dbl;
    return S_OK;
}

static HRESULT push_instr_uint(compiler_ctx_t *ctx, jsop_t op, unsigned arg)
{
    unsigned instr;

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.uint = arg;
    return S_OK;
}

static HRESULT compile_binary_expression(compiler_ctx_t *ctx, binary_expression_t *expr, jsop_t op)
{
    HRESULT hres;

    hres = compile_expression(ctx, expr->expression1);
    if(FAILED(hres))
        return hres;

    hres = compile_expression(ctx, expr->expression2);
    if(FAILED(hres))
        return hres;

    return push_instr(ctx, op) == -1 ? E_OUTOFMEMORY : S_OK;
}

static HRESULT compile_unary_expression(compiler_ctx_t *ctx, unary_expression_t *expr, jsop_t op)
{
    HRESULT hres;

    hres = compile_expression(ctx, expr->expression);
    if(FAILED(hres))
        return hres;

    return push_instr(ctx, op) == -1 ? E_OUTOFMEMORY : S_OK;
}

/* ECMA-262 3rd Edition    11.2.1 */
static HRESULT compile_member_expression(compiler_ctx_t *ctx, member_expression_t *expr)
{
    HRESULT hres;

    hres = compile_expression(ctx, expr->expression);
    if(FAILED(hres))
        return hres;

    return push_instr_bstr(ctx, OP_member, expr->identifier);
}

#define LABEL_FLAG 0x80000000

static unsigned alloc_label(compiler_ctx_t *ctx)
{
    if(!ctx->labels_size) {
        ctx->labels = heap_alloc(8 * sizeof(*ctx->labels));
        if(!ctx->labels)
            return -1;
        ctx->labels_size = 8;
    }else if(ctx->labels_size == ctx->labels_cnt) {
        unsigned *new_labels;

        new_labels = heap_realloc(ctx->labels, 2*ctx->labels_size*sizeof(*ctx->labels));
        if(!new_labels)
            return -1;

        ctx->labels = new_labels;
        ctx->labels_size *= 2;
    }

    return ctx->labels_cnt++ | LABEL_FLAG;
}

static void label_set_addr(compiler_ctx_t *ctx, unsigned label)
{
    assert(label & LABEL_FLAG);
    ctx->labels[label & ~LABEL_FLAG] = ctx->code_off;
}

static inline BOOL is_memberid_expr(expression_type_t type)
{
    return type == EXPR_IDENT || type == EXPR_MEMBER || type == EXPR_ARRAY;
}

static HRESULT compile_memberid_expression(compiler_ctx_t *ctx, expression_t *expr, unsigned flags)
{
    HRESULT hres = S_OK;

    switch(expr->type) {
    case EXPR_IDENT: {
        identifier_expression_t *ident_expr = (identifier_expression_t*)expr;

        hres = push_instr_bstr_uint(ctx, OP_identid, ident_expr->identifier, flags);
        break;
    }
    case EXPR_ARRAY: {
        binary_expression_t *array_expr = (binary_expression_t*)expr;

        hres = compile_expression(ctx, array_expr->expression1);
        if(FAILED(hres))
            return hres;

        hres = compile_expression(ctx, array_expr->expression2);
        if(FAILED(hres))
            return hres;

        hres = push_instr_uint(ctx, OP_memberid, flags);
        break;
    }
    case EXPR_MEMBER: {
        member_expression_t *member_expr = (member_expression_t*)expr;

        hres = compile_expression(ctx, member_expr->expression);
        if(FAILED(hres))
            return hres;

        /* FIXME: Potential optimization */
        hres = push_instr_str(ctx, OP_str, member_expr->identifier);
        if(FAILED(hres))
            return hres;

        hres = push_instr_uint(ctx, OP_memberid, flags);
        break;
    }
    default:
        assert(0);
    }

    return hres;
}

static HRESULT compile_increment_expression(compiler_ctx_t *ctx, unary_expression_t *expr, jsop_t op, int n)
{
    HRESULT hres;

    if(!is_memberid_expr(expr->expression->type)) {
        hres = compile_expression(ctx, expr->expression);
        if(FAILED(hres))
            return hres;

        return push_instr_uint(ctx, OP_throw_ref, JS_E_ILLEGAL_ASSIGN);
    }

    hres = compile_memberid_expression(ctx, expr->expression, fdexNameEnsure);
    if(FAILED(hres))
        return hres;

    return push_instr_int(ctx, op, n);
}

/* ECMA-262 3rd Edition    11.14 */
static HRESULT compile_comma_expression(compiler_ctx_t *ctx, binary_expression_t *expr)
{
    HRESULT hres;

    hres = compile_expression(ctx, expr->expression1);
    if(FAILED(hres))
        return hres;

    if(push_instr(ctx, OP_pop) == -1)
        return E_OUTOFMEMORY;

    return compile_expression(ctx, expr->expression2);
}

/* ECMA-262 3rd Edition    11.11 */
static HRESULT compile_logical_expression(compiler_ctx_t *ctx, binary_expression_t *expr, jsop_t op)
{
    unsigned instr;
    HRESULT hres;

    hres = compile_expression(ctx, expr->expression1);
    if(FAILED(hres))
        return hres;

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    hres = compile_expression(ctx, expr->expression2);
    if(FAILED(hres))
        return hres;

    instr_ptr(ctx, instr)->arg1.uint = ctx->code_off;
    return S_OK;
}

/* ECMA-262 3rd Edition    11.12 */
static HRESULT compile_conditional_expression(compiler_ctx_t *ctx, conditional_expression_t *expr)
{
    unsigned jmp_false, jmp_end;
    HRESULT hres;

    hres = compile_expression(ctx, expr->expression);
    if(FAILED(hres))
        return hres;

    jmp_false = push_instr(ctx, OP_cnd_z);
    if(jmp_false == -1)
        return E_OUTOFMEMORY;

    hres = compile_expression(ctx, expr->true_expression);
    if(FAILED(hres))
        return hres;

    jmp_end = push_instr(ctx, OP_jmp);
    if(jmp_end == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, jmp_false)->arg1.uint = ctx->code_off;
    if(push_instr(ctx, OP_pop) == -1)
        return E_OUTOFMEMORY;

    hres = compile_expression(ctx, expr->false_expression);
    if(FAILED(hres))
        return hres;

    instr_ptr(ctx, jmp_end)->arg1.uint = ctx->code_off;
    return S_OK;
}

static HRESULT compile_new_expression(compiler_ctx_t *ctx, call_expression_t *expr)
{
    unsigned arg_cnt = 0;
    argument_t *arg;
    HRESULT hres;

    hres = compile_expression(ctx, expr->expression);
    if(FAILED(hres))
        return hres;

    for(arg = expr->argument_list; arg; arg = arg->next) {
        hres = compile_expression(ctx, arg->expr);
        if(FAILED(hres))
            return hres;
        arg_cnt++;
    }

    return push_instr_int(ctx, OP_new, arg_cnt);
}

static HRESULT compile_interp_fallback(compiler_ctx_t *ctx, statement_t *stat)
{
    unsigned instr;

    if(ctx->no_fallback)
        return E_NOTIMPL;

    instr = push_instr(ctx, OP_tree);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.stat = stat;
    return S_OK;
}

static HRESULT compile_call_expression(compiler_ctx_t *ctx, call_expression_t *expr, BOOL *no_ret)
{
    unsigned arg_cnt = 0;
    argument_t *arg;
    unsigned instr;
    jsop_t op;
    HRESULT hres;

    if(is_memberid_expr(expr->expression->type)) {
        op = OP_call_member;
        hres = compile_memberid_expression(ctx, expr->expression, 0);
    }else {
        op = OP_call;
        hres = compile_expression(ctx, expr->expression);
    }

    if(FAILED(hres))
        return hres;

    for(arg = expr->argument_list; arg; arg = arg->next) {
        hres = compile_expression(ctx, arg->expr);
        if(FAILED(hres))
            return hres;
        arg_cnt++;
    }

    instr = push_instr(ctx, op);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.uint = arg_cnt;
    instr_ptr(ctx, instr)->arg2.lng = no_ret == NULL;
    if(no_ret)
        *no_ret = TRUE;
    return S_OK;
}

static HRESULT compile_delete_expression(compiler_ctx_t *ctx, unary_expression_t *expr)
{
    HRESULT hres;

    switch(expr->expression->type) {
    case EXPR_ARRAY: {
        binary_expression_t *array_expr = (binary_expression_t*)expr->expression;

        hres = compile_expression(ctx, array_expr->expression1);
        if(FAILED(hres))
            return hres;

        hres = compile_expression(ctx, array_expr->expression2);
        if(FAILED(hres))
            return hres;

        if(push_instr(ctx, OP_delete) == -1)
            return E_OUTOFMEMORY;
        break;
    }
    case EXPR_MEMBER: {
        member_expression_t *member_expr = (member_expression_t*)expr->expression;

        hres = compile_expression(ctx, member_expr->expression);
        if(FAILED(hres))
            return hres;

        /* FIXME: Potential optimization */
        hres = push_instr_str(ctx, OP_str, member_expr->identifier);
        if(FAILED(hres))
            return hres;

        if(push_instr(ctx, OP_delete) == -1)
            return E_OUTOFMEMORY;
        break;
    }
    case EXPR_IDENT:
        return push_instr_bstr(ctx, OP_delete_ident, ((identifier_expression_t*)expr->expression)->identifier);
    default: {
        const WCHAR fixmeW[] = {'F','I','X','M','E',0};

        WARN("invalid delete, unimplemented exception message\n");

        hres = compile_expression(ctx, expr->expression);
        if(FAILED(hres))
            return hres;

        return push_instr_uint_str(ctx, OP_throw_type, JS_E_INVALID_DELETE, fixmeW);
    }
    }

    return S_OK;
}

static HRESULT compile_assign_expression(compiler_ctx_t *ctx, binary_expression_t *expr, jsop_t op)
{
    HRESULT hres;

    if(!is_memberid_expr(expr->expression1->type)) {
        hres = compile_expression(ctx, expr->expression1);
        if(FAILED(hres))
            return hres;

        hres = compile_expression(ctx, expr->expression2);
        if(FAILED(hres))
            return hres;

        if(op != OP_LAST && push_instr(ctx, op) == -1)
            return E_OUTOFMEMORY;

        return push_instr_uint(ctx, OP_throw_ref, JS_E_ILLEGAL_ASSIGN);
    }

    hres = compile_memberid_expression(ctx, expr->expression1, fdexNameEnsure);
    if(FAILED(hres))
        return hres;

    if(op != OP_LAST && push_instr(ctx, OP_refval) == -1)
        return E_OUTOFMEMORY;

    hres = compile_expression(ctx, expr->expression2);
    if(FAILED(hres))
        return hres;

    if(op != OP_LAST && push_instr(ctx, op) == -1)
        return E_OUTOFMEMORY;

    if(push_instr(ctx, OP_assign) == -1)
        return E_OUTOFMEMORY;

    return S_OK;
}

static HRESULT compile_typeof_expression(compiler_ctx_t *ctx, unary_expression_t *expr)
{
    jsop_t op;
    HRESULT hres;

    if(is_memberid_expr(expr->expression->type)) {
        if(expr->expression->type == EXPR_IDENT)
            return push_instr_str(ctx, OP_typeofident, ((identifier_expression_t*)expr->expression)->identifier);

        op = OP_typeofid;
        hres = compile_memberid_expression(ctx, expr->expression, 0);
    }else {
        op = OP_typeof;
        hres = compile_expression(ctx, expr->expression);
    }
    if(FAILED(hres))
        return hres;

    return push_instr(ctx, op) == -1 ? E_OUTOFMEMORY : S_OK;
}

static HRESULT compile_literal(compiler_ctx_t *ctx, literal_t *literal)
{
    switch(literal->type) {
    case LT_BOOL:
        return push_instr_int(ctx, OP_bool, literal->u.bval);
    case LT_DOUBLE:
        return push_instr_double(ctx, OP_double, literal->u.dval);
    case LT_INT:
        return push_instr_int(ctx, OP_int, literal->u.lval);
    case LT_NULL:
        return push_instr(ctx, OP_null);
    case LT_STRING:
        return push_instr_str(ctx, OP_str, literal->u.wstr);
    case LT_REGEXP: {
        unsigned instr;
        WCHAR *str;

        str = compiler_alloc(ctx->code, (literal->u.regexp.str_len+1)*sizeof(WCHAR));
        if(!str)
            return E_OUTOFMEMORY;
        memcpy(str, literal->u.regexp.str, literal->u.regexp.str_len*sizeof(WCHAR));
        str[literal->u.regexp.str_len] = 0;

        instr = push_instr(ctx, OP_regexp);
        if(instr == -1)
            return E_OUTOFMEMORY;

        instr_ptr(ctx, instr)->arg1.str = str;
        instr_ptr(ctx, instr)->arg2.lng = literal->u.regexp.flags;
        return S_OK;
    }
    default:
        assert(0);
    }
}

static HRESULT literal_as_bstr(compiler_ctx_t *ctx, literal_t *literal, BSTR *str)
{
    switch(literal->type) {
    case LT_STRING:
        *str = compiler_alloc_bstr(ctx, literal->u.wstr);
        break;
    case LT_INT:
        *str = int_to_bstr(literal->u.lval);
        break;
    case LT_DOUBLE:
        return double_to_bstr(literal->u.dval, str);
    default:
        assert(0);
    }

    return *str ? S_OK : E_OUTOFMEMORY;
}

static HRESULT compile_array_literal(compiler_ctx_t *ctx, array_literal_expression_t *expr)
{
    unsigned i, elem_cnt = expr->length;
    array_element_t *iter;
    HRESULT hres;

    for(iter = expr->element_list; iter; iter = iter->next) {
        elem_cnt += iter->elision+1;

        for(i=0; i < iter->elision; i++) {
            if(push_instr(ctx, OP_undefined) == -1)
                return E_OUTOFMEMORY;
        }

        hres = compile_expression(ctx, iter->expr);
        if(FAILED(hres))
            return hres;
    }

    for(i=0; i < expr->length; i++) {
        if(push_instr(ctx, OP_undefined) == -1)
            return E_OUTOFMEMORY;
    }

    return push_instr_uint(ctx, OP_carray, elem_cnt);
}

static HRESULT compile_object_literal(compiler_ctx_t *ctx, property_value_expression_t *expr)
{
    prop_val_t *iter;
    unsigned instr;
    BSTR name;
    HRESULT hres;

    if(push_instr(ctx, OP_new_obj) == -1)
        return E_OUTOFMEMORY;

    for(iter = expr->property_list; iter; iter = iter->next) {
        hres = literal_as_bstr(ctx, iter->name, &name);
        if(FAILED(hres))
            return hres;

        hres = compile_expression(ctx, iter->value);
        if(FAILED(hres))
            return hres;

        instr = push_instr(ctx, OP_obj_prop);
        if(instr == -1)
            return E_OUTOFMEMORY;

        instr_ptr(ctx, instr)->arg1.bstr = name;
    }

    return S_OK;
}

static HRESULT compile_function_expression(compiler_ctx_t *ctx, function_expression_t *expr)
{
    unsigned instr;

    /* FIXME: not exactly right */
    if(expr->identifier)
        return push_instr_bstr(ctx, OP_ident, expr->identifier);

    instr = push_instr(ctx, OP_func);
    if(instr == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, instr)->arg1.func = expr;
    return S_OK;
}

static HRESULT compile_expression_noret(compiler_ctx_t *ctx, expression_t *expr, BOOL *no_ret)
{
    switch(expr->type) {
    case EXPR_ADD:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_add);
    case EXPR_AND:
        return compile_logical_expression(ctx, (binary_expression_t*)expr, OP_cnd_z);
    case EXPR_ARRAY:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_array);
    case EXPR_ARRAYLIT:
        return compile_array_literal(ctx, (array_literal_expression_t*)expr);
    case EXPR_ASSIGN:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_LAST);
    case EXPR_ASSIGNADD:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_add);
    case EXPR_ASSIGNAND:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_and);
    case EXPR_ASSIGNSUB:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_sub);
    case EXPR_ASSIGNMUL:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_mul);
    case EXPR_ASSIGNDIV:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_div);
    case EXPR_ASSIGNMOD:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_mod);
    case EXPR_ASSIGNOR:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_or);
    case EXPR_ASSIGNLSHIFT:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_lshift);
    case EXPR_ASSIGNRSHIFT:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_rshift);
    case EXPR_ASSIGNRRSHIFT:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_rshift2);
    case EXPR_ASSIGNXOR:
        return compile_assign_expression(ctx, (binary_expression_t*)expr, OP_xor);
    case EXPR_BAND:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_and);
    case EXPR_BITNEG:
        return compile_unary_expression(ctx, (unary_expression_t*)expr, OP_bneg);
    case EXPR_BOR:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_or);
    case EXPR_CALL:
        return compile_call_expression(ctx, (call_expression_t*)expr, no_ret);
    case EXPR_COMMA:
        return compile_comma_expression(ctx, (binary_expression_t*)expr);
    case EXPR_COND:
        return compile_conditional_expression(ctx, (conditional_expression_t*)expr);
    case EXPR_DELETE:
        return compile_delete_expression(ctx, (unary_expression_t*)expr);
    case EXPR_DIV:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_div);
    case EXPR_EQ:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_eq);
    case EXPR_EQEQ:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_eq2);
    case EXPR_FUNC:
        return compile_function_expression(ctx, (function_expression_t*)expr);
    case EXPR_GREATER:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_gt);
    case EXPR_GREATEREQ:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_gteq);
    case EXPR_IDENT:
        return push_instr_bstr(ctx, OP_ident, ((identifier_expression_t*)expr)->identifier);
    case EXPR_IN:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_in);
    case EXPR_INSTANCEOF:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_instanceof);
    case EXPR_LESS:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_lt);
    case EXPR_LESSEQ:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_lteq);
    case EXPR_LITERAL:
        return compile_literal(ctx, ((literal_expression_t*)expr)->literal);
    case EXPR_LOGNEG:
        return compile_unary_expression(ctx, (unary_expression_t*)expr, OP_neg);
    case EXPR_LSHIFT:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_lshift);
    case EXPR_MEMBER:
        return compile_member_expression(ctx, (member_expression_t*)expr);
    case EXPR_MINUS:
        return compile_unary_expression(ctx, (unary_expression_t*)expr, OP_minus);
    case EXPR_MOD:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_mod);
    case EXPR_MUL:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_mul);
    case EXPR_NEW:
        return compile_new_expression(ctx, (call_expression_t*)expr);
    case EXPR_NOTEQ:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_neq);
    case EXPR_NOTEQEQ:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_neq2);
    case EXPR_OR:
        return compile_logical_expression(ctx, (binary_expression_t*)expr, OP_cnd_nz);
    case EXPR_PLUS:
        return compile_unary_expression(ctx, (unary_expression_t*)expr, OP_tonum);
    case EXPR_POSTDEC:
        return compile_increment_expression(ctx, (unary_expression_t*)expr, OP_postinc, -1);
    case EXPR_POSTINC:
        return compile_increment_expression(ctx, (unary_expression_t*)expr, OP_postinc, 1);
    case EXPR_PREDEC:
        return compile_increment_expression(ctx, (unary_expression_t*)expr, OP_preinc, -1);
    case EXPR_PREINC:
        return compile_increment_expression(ctx, (unary_expression_t*)expr, OP_preinc, 1);
    case EXPR_PROPVAL:
        return compile_object_literal(ctx, (property_value_expression_t*)expr);
    case EXPR_RSHIFT:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_rshift);
    case EXPR_RRSHIFT:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_rshift2);
    case EXPR_SUB:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_sub);
    case EXPR_THIS:
        return push_instr(ctx, OP_this) == -1 ? E_OUTOFMEMORY : S_OK;
    case EXPR_TYPEOF:
        return compile_typeof_expression(ctx, (unary_expression_t*)expr);
    case EXPR_VOID:
        return compile_unary_expression(ctx, (unary_expression_t*)expr, OP_void);
    case EXPR_BXOR:
        return compile_binary_expression(ctx, (binary_expression_t*)expr, OP_xor);
    default:
        assert(0);
    }

    return S_OK;
}

static HRESULT compile_expression(compiler_ctx_t *ctx, expression_t *expr)
{
    return compile_expression_noret(ctx, expr, NULL);
}

/* ECMA-262 3rd Edition    12.1 */
static HRESULT compile_block_statement(compiler_ctx_t *ctx, statement_t *iter)
{
    HRESULT hres;

    /* FIXME: do it only if needed */
    if(!iter)
        return push_instr(ctx, OP_undefined) == -1 ? E_OUTOFMEMORY : S_OK;

    while(1) {
        hres = compile_statement(ctx, NULL, iter);
        if(FAILED(hres))
            return hres;

        iter = iter->next;
        if(!iter)
            break;

        if(push_instr(ctx, OP_pop) == -1)
            return E_OUTOFMEMORY;
    }

    return S_OK;
}

/* ECMA-262 3rd Edition    12.2 */
static HRESULT compile_variable_list(compiler_ctx_t *ctx, variable_declaration_t *list)
{
    variable_declaration_t *iter;
    HRESULT hres;

    for(iter = list; iter; iter = iter->next) {
        if(!iter->expr)
            continue;

        hres = compile_expression(ctx, iter->expr);
        if(FAILED(hres))
            return hres;

        hres = push_instr_bstr(ctx, OP_var_set, iter->identifier);
        if(FAILED(hres))
            return hres;
    }

    return S_OK;
}

/* ECMA-262 3rd Edition    12.2 */
static HRESULT compile_var_statement(compiler_ctx_t *ctx, var_statement_t *stat)
{
    HRESULT hres;

    hres = compile_variable_list(ctx, stat->variable_list);
    if(FAILED(hres))
        return hres;

    return push_instr(ctx, OP_undefined) == -1 ? E_OUTOFMEMORY : S_OK;
}

/* ECMA-262 3rd Edition    12.4 */
static HRESULT compile_expression_statement(compiler_ctx_t *ctx, expression_statement_t *stat)
{
    BOOL no_ret = FALSE;
    HRESULT hres;

    hres = compile_expression_noret(ctx, stat->expr, &no_ret);
    if(FAILED(hres))
        return hres;

    /* FIXME: that's a big potential optimization */
    if(no_ret && !push_instr(ctx, OP_undefined) == -1)
        return E_OUTOFMEMORY;

    return S_OK;
}

/* ECMA-262 3rd Edition    12.5 */
static HRESULT compile_if_statement(compiler_ctx_t *ctx, if_statement_t *stat)
{
    unsigned jmp_else, jmp_end;
    HRESULT hres;

    hres = compile_expression(ctx, stat->expr);
    if(FAILED(hres))
        return hres;

    jmp_else = push_instr(ctx, OP_jmp_z);
    if(jmp_else == -1)
        return E_OUTOFMEMORY;

    hres = compile_statement(ctx, NULL, stat->if_stat);
    if(FAILED(hres))
        return hres;

    jmp_end = push_instr(ctx, OP_jmp);
    if(jmp_end == -1)
        return E_OUTOFMEMORY;

    instr_ptr(ctx, jmp_else)->arg1.uint = ctx->code_off;

    if(stat->else_stat) {
        hres = compile_statement(ctx, NULL, stat->else_stat);
        if(FAILED(hres))
            return hres;
    }else {
        /* FIXME: We could sometimes avoid it */
        if(push_instr(ctx, OP_undefined) == -1)
            return E_OUTOFMEMORY;
    }

    instr_ptr(ctx, jmp_end)->arg1.uint = ctx->code_off;
    return S_OK;
}

/* ECMA-262 3rd Edition    12.6.2 */
static HRESULT compile_while_statement(compiler_ctx_t *ctx, while_statement_t *stat)
{
    statement_ctx_t stat_ctx = {0, FALSE, FALSE};
    unsigned off_backup, jmp_off;
    BOOL prev_no_fallback;
    HRESULT hres;

    off_backup = ctx->code_off;

    stat_ctx.break_label = alloc_label(ctx);
    if(stat_ctx.break_label == -1)
        return E_OUTOFMEMORY;

    stat_ctx.continue_label = alloc_label(ctx);
    if(stat_ctx.continue_label == -1)
        return E_OUTOFMEMORY;

    if(!stat->do_while) {
        /* FIXME: avoid */
        if(push_instr(ctx, OP_undefined) == -1)
            return E_OUTOFMEMORY;

        jmp_off = ctx->code_off;
        label_set_addr(ctx, stat_ctx.continue_label);
        hres = compile_expression(ctx, stat->expr);
        if(FAILED(hres))
            return hres;

        hres = push_instr_uint(ctx, OP_jmp_z, stat_ctx.break_label);
        if(FAILED(hres))
            return hres;

        if(push_instr(ctx, OP_pop) == -1)
            return E_OUTOFMEMORY;
    }else {
        jmp_off = ctx->code_off;
    }

    prev_no_fallback = ctx->no_fallback;
    ctx->no_fallback = TRUE;
    hres = compile_statement(ctx, &stat_ctx, stat->statement);
    ctx->no_fallback = prev_no_fallback;
    if(hres == E_NOTIMPL) {
        ctx->code_off = off_backup;
        stat->stat.eval = while_statement_eval;
        return compile_interp_fallback(ctx, &stat->stat);
    }
    if(FAILED(hres))
        return hres;

    if(stat->do_while) {
        label_set_addr(ctx, stat_ctx.continue_label);
        hres = compile_expression(ctx, stat->expr);
        if(FAILED(hres))
            return hres;

        hres = push_instr_uint(ctx, OP_jmp_z, stat_ctx.break_label);
        if(FAILED(hres))
            return hres;

        if(push_instr(ctx, OP_pop) == -1)
            return E_OUTOFMEMORY;
    }

    hres = push_instr_uint(ctx, OP_jmp, jmp_off);
    if(FAILED(hres))
        return hres;

    label_set_addr(ctx, stat_ctx.break_label);
    return S_OK;
}

/* ECMA-262 3rd Edition    12.6.3 */
static HRESULT compile_for_statement(compiler_ctx_t *ctx, for_statement_t *stat)
{
    statement_ctx_t stat_ctx = {0, FALSE, FALSE};
    unsigned off_backup, expr_off;
    BOOL prev_no_fallback;
    HRESULT hres;

    off_backup = ctx->code_off;

    if(stat->variable_list) {
        hres = compile_variable_list(ctx, stat->variable_list);
        if(FAILED(hres))
            return hres;
    }else if(stat->begin_expr) {
        BOOL no_ret = FALSE;

        hres = compile_expression_noret(ctx, stat->begin_expr, &no_ret);
        if(FAILED(hres))
            return hres;
        if(!no_ret && push_instr(ctx, OP_pop) == -1)
            return E_OUTOFMEMORY;
    }

    stat_ctx.break_label = alloc_label(ctx);
    if(stat_ctx.break_label == -1)
        return E_OUTOFMEMORY;

    stat_ctx.continue_label = alloc_label(ctx);
    if(stat_ctx.continue_label == -1)
        return E_OUTOFMEMORY;

    /* FIXME: avoid */
    if(push_instr(ctx, OP_undefined) == -1)
        return E_OUTOFMEMORY;

    expr_off = ctx->code_off;

    if(stat->expr) {
        hres = compile_expression(ctx, stat->expr);
        if(FAILED(hres))
            return hres;

        hres = push_instr_uint(ctx, OP_jmp_z, stat_ctx.break_label);
        if(FAILED(hres))
            return hres;
    }

    if(push_instr(ctx, OP_pop) == -1)
        return E_OUTOFMEMORY;

    prev_no_fallback = ctx->no_fallback;
    ctx->no_fallback = TRUE;
    hres = compile_statement(ctx, &stat_ctx, stat->statement);
    ctx->no_fallback = prev_no_fallback;
    if(hres == E_NOTIMPL) {
        ctx->code_off = off_backup;
        stat->stat.eval = for_statement_eval;
        return compile_interp_fallback(ctx, &stat->stat);
    }
    if(FAILED(hres))
        return hres;

    label_set_addr(ctx, stat_ctx.continue_label);

    if(stat->end_expr) {
        BOOL no_ret = FALSE;

        hres = compile_expression_noret(ctx, stat->end_expr, &no_ret);
        if(FAILED(hres))
            return hres;

        if(!no_ret && push_instr(ctx, OP_pop) == -1)
            return E_OUTOFMEMORY;
    }

    hres = push_instr_uint(ctx, OP_jmp, expr_off);
    if(FAILED(hres))
        return hres;

    label_set_addr(ctx, stat_ctx.break_label);
    return S_OK;
}

/* ECMA-262 3rd Edition    12.6.4 */
static HRESULT compile_forin_statement(compiler_ctx_t *ctx, forin_statement_t *stat)
{
    statement_ctx_t stat_ctx = {4, FALSE, FALSE};
    unsigned off_backup = ctx->code_off;
    BOOL prev_no_fallback;
    HRESULT hres;

    if(stat->variable) {
        hres = compile_variable_list(ctx, stat->variable);
        if(FAILED(hres))
            return hres;
    }

    stat_ctx.break_label = alloc_label(ctx);
    if(stat_ctx.break_label == -1)
        return E_OUTOFMEMORY;

    stat_ctx.continue_label = alloc_label(ctx);
    if(stat_ctx.continue_label == -1)
        return E_OUTOFMEMORY;

    hres = compile_expression(ctx, stat->in_expr);
    if(FAILED(hres))
        return hres;

    if(stat->variable) {
        hres = push_instr_bstr_uint(ctx, OP_identid, stat->variable->identifier, fdexNameEnsure);
        if(FAILED(hres))
            return hres;
    }else if(is_memberid_expr(stat->expr->type)) {
        hres = compile_memberid_expression(ctx, stat->expr, fdexNameEnsure);
        if(FAILED(hres))
            return hres;
    }else {
        hres = push_instr_uint(ctx, OP_throw_ref, JS_E_ILLEGAL_ASSIGN);
        if(FAILED(hres))
            return hres;

        /* FIXME: compile statement anyways when we depend on compiler to check errors */
        return S_OK;
    }

    hres = push_instr_int(ctx, OP_int, DISPID_STARTENUM);
    if(FAILED(hres))
        return hres;

    /* FIXME: avoid */
    if(push_instr(ctx, OP_undefined) == -1)
        return E_OUTOFMEMORY;

    label_set_addr(ctx, stat_ctx.continue_label);
    hres = push_instr_uint(ctx, OP_forin, stat_ctx.break_label);
    if(FAILED(hres))
        return E_OUTOFMEMORY;

    prev_no_fallback = ctx->no_fallback;
    ctx->no_fallback = TRUE;
    hres = compile_statement(ctx, &stat_ctx, stat->statement);
    ctx->no_fallback = prev_no_fallback;
    if(hres == E_NOTIMPL) {
        ctx->code_off = off_backup;
        stat->stat.eval = forin_statement_eval;
        return compile_interp_fallback(ctx, &stat->stat);
    }
    if(FAILED(hres))
        return hres;

    hres = push_instr_uint(ctx, OP_jmp, stat_ctx.continue_label);
    if(FAILED(hres))
        return hres;

    label_set_addr(ctx, stat_ctx.break_label);
    return S_OK;
}

static HRESULT pop_to_stat(compiler_ctx_t *ctx, statement_ctx_t *stat_ctx)
{
    statement_ctx_t *iter = ctx->stat_ctx;
    unsigned stack_pop = 0;

    while(1) {
        if(iter->using_scope && push_instr(ctx, OP_pop_scope) == -1)
            return E_OUTOFMEMORY;
        if(iter->using_except && push_instr(ctx, OP_pop_except) == -1)
            return E_OUTOFMEMORY;
        stack_pop += iter->stack_use;
        if(iter == stat_ctx)
            break;
        iter = iter->next;
    }

    /* FIXME: optimize */
    while(stack_pop--) {
        if(push_instr(ctx, OP_pop) == -1)
            return E_OUTOFMEMORY;
    }

    return S_OK;
}

/* ECMA-262 3rd Edition    12.7 */
static HRESULT compile_continue_statement(compiler_ctx_t *ctx, branch_statement_t *stat)
{
    statement_ctx_t *pop_ctx;
    HRESULT hres;

    for(pop_ctx = ctx->stat_ctx; pop_ctx; pop_ctx = pop_ctx->next) {
        if(pop_ctx->continue_label != -1)
            break;
    }

    if(!pop_ctx || stat->identifier) {
        stat->stat.eval = continue_statement_eval;
        return compile_interp_fallback(ctx, &stat->stat);
    }

    hres = pop_to_stat(ctx, pop_ctx);
    if(FAILED(hres))
        return hres;

    if(push_instr(ctx, OP_undefined) == -1)
        return E_OUTOFMEMORY;

    return push_instr_uint(ctx, OP_jmp, pop_ctx->continue_label);
}

/* ECMA-262 3rd Edition    12.8 */
static HRESULT compile_break_statement(compiler_ctx_t *ctx, branch_statement_t *stat)
{
    statement_ctx_t *pop_ctx;
    HRESULT hres;

    for(pop_ctx = ctx->stat_ctx; pop_ctx; pop_ctx = pop_ctx->next) {
        if(pop_ctx->break_label != -1)
            break;
    }

    if(!pop_ctx || stat->identifier) {
        stat->stat.eval = break_statement_eval;
        return compile_interp_fallback(ctx, &stat->stat);
    }

    hres = pop_to_stat(ctx, pop_ctx);
    if(FAILED(hres))
        return hres;

    if(push_instr(ctx, OP_undefined) == -1)
        return E_OUTOFMEMORY;

    return push_instr_uint(ctx, OP_jmp, pop_ctx->break_label);
}

/* ECMA-262 3rd Edition    12.10 */
static HRESULT compile_with_statement(compiler_ctx_t *ctx, with_statement_t *stat)
{
    statement_ctx_t stat_ctx = {0, TRUE, FALSE, -1, -1};
    unsigned off_backup;
    BOOL prev_no_fallback;
    HRESULT hres;

    off_backup = ctx->code_off;

    hres = compile_expression(ctx, stat->expr);
    if(FAILED(hres))
        return hres;

    if(push_instr(ctx, OP_push_scope) == -1)
        return E_OUTOFMEMORY;

    prev_no_fallback = ctx->no_fallback;
    ctx->no_fallback = TRUE;
    hres = compile_statement(ctx, &stat_ctx, stat->statement);
    ctx->no_fallback = prev_no_fallback;
    if(hres == E_NOTIMPL) {
        ctx->code_off = off_backup;
        stat->stat.eval = with_statement_eval;
        return compile_interp_fallback(ctx, &stat->stat);
    }
    if(FAILED(hres))
        return hres;

    if(push_instr(ctx, OP_pop_scope) == -1)
        return E_OUTOFMEMORY;

    return S_OK;
}

/* ECMA-262 3rd Edition    12.13 */
static HRESULT compile_switch_statement(compiler_ctx_t *ctx, switch_statement_t *stat)
{
    statement_ctx_t stat_ctx = {0, FALSE, FALSE, -1, -1};
    unsigned case_cnt = 0, *case_jmps, i, default_jmp;
    BOOL have_default = FALSE;
    statement_t *stat_iter;
    case_clausule_t *iter;
    unsigned off_backup;
    BOOL prev_no_fallback;
    HRESULT hres;

    off_backup = ctx->code_off;

    hres = compile_expression(ctx, stat->expr);
    if(FAILED(hres))
        return hres;

    stat_ctx.break_label = alloc_label(ctx);
    if(stat_ctx.break_label == -1)
        return E_OUTOFMEMORY;

    for(iter = stat->case_list; iter; iter = iter->next) {
        if(iter->expr)
            case_cnt++;
    }

    case_jmps = heap_alloc(case_cnt * sizeof(*case_jmps));
    if(!case_jmps)
        return E_OUTOFMEMORY;

    i = 0;
    for(iter = stat->case_list; iter; iter = iter->next) {
        if(!iter->expr) {
            have_default = TRUE;
            continue;
        }

        hres = compile_expression(ctx, iter->expr);
        if(FAILED(hres))
            break;

        case_jmps[i] = push_instr(ctx, OP_case);
        if(case_jmps[i] == -1) {
            hres = E_OUTOFMEMORY;
            break;
        }
        i++;
    }

    if(SUCCEEDED(hres)) {
        if(push_instr(ctx, OP_pop) != -1) {
            default_jmp = push_instr(ctx, OP_jmp);
            if(default_jmp == -1)
                hres = E_OUTOFMEMORY;
        }else {
            hres = E_OUTOFMEMORY;
        }
    }

    if(FAILED(hres)) {
        heap_free(case_jmps);
        return hres;
    }

    i = 0;
    for(iter = stat->case_list; iter; iter = iter->next) {
        while(iter->next && iter->next->stat == iter->stat) {
            instr_ptr(ctx, iter->expr ? case_jmps[i++] : default_jmp)->arg1.uint = ctx->code_off;
            iter = iter->next;
        }

        instr_ptr(ctx, iter->expr ? case_jmps[i++] : default_jmp)->arg1.uint = ctx->code_off;

        for(stat_iter = iter->stat; stat_iter && (!iter->next || iter->next->stat != stat_iter); stat_iter = stat_iter->next) {
            prev_no_fallback = ctx->no_fallback;
            ctx->no_fallback = TRUE;
            hres = compile_statement(ctx, &stat_ctx, stat_iter);
            ctx->no_fallback = prev_no_fallback;
            if(hres == E_NOTIMPL) {
                ctx->code_off = off_backup;
                stat->stat.eval = switch_statement_eval;
                return compile_interp_fallback(ctx, &stat->stat);
            }
            if(FAILED(hres))
                break;

            if(stat_iter->next && push_instr(ctx, OP_pop) == -1) {
                hres = E_OUTOFMEMORY;
                break;
            }
        }
        if(FAILED(hres))
            break;
    }

    heap_free(case_jmps);
    if(FAILED(hres))
        return hres;
    assert(i == case_cnt);

    if(!have_default)
        instr_ptr(ctx, default_jmp)->arg1.uint = ctx->code_off;

    label_set_addr(ctx, stat_ctx.break_label);
    return S_OK;
}

/* ECMA-262 3rd Edition    12.13 */
static HRESULT compile_throw_statement(compiler_ctx_t *ctx, expression_statement_t *stat)
{
    HRESULT hres;

    hres = compile_expression(ctx, stat->expr);
    if(FAILED(hres))
        return hres;

    return push_instr(ctx, OP_throw) == -1 ? E_OUTOFMEMORY : S_OK;
}

/* ECMA-262 3rd Edition    12.14 */
static HRESULT compile_try_statement(compiler_ctx_t *ctx, try_statement_t *stat)
{
    statement_ctx_t try_ctx = {0, FALSE, TRUE, -1, -1}, catch_ctx = {0, TRUE, FALSE, -1, -1};
    statement_ctx_t finally_ctx = {2, FALSE, FALSE, -1, -1};
    unsigned off_backup, push_except;
    BOOL prev_no_fallback;
    BSTR ident;
    HRESULT hres;

    off_backup = ctx->code_off;
    prev_no_fallback = ctx->no_fallback;

    push_except = push_instr(ctx, OP_push_except);
    if(push_except == -1)
        return E_OUTOFMEMORY;

    if(stat->catch_block) {
        ident = compiler_alloc_bstr(ctx, stat->catch_block->identifier);
        if(!ident)
            return E_OUTOFMEMORY;
    }else {
        ident = NULL;
    }

    instr_ptr(ctx, push_except)->arg2.bstr = ident;

    if(!stat->catch_block)
        try_ctx.stack_use = 2;

    ctx->no_fallback = TRUE;
    hres = compile_statement(ctx, &try_ctx, stat->try_statement);
    ctx->no_fallback = prev_no_fallback;
    if(hres == E_NOTIMPL) {
        ctx->code_off = off_backup;
        stat->stat.eval = try_statement_eval;
        return compile_interp_fallback(ctx, &stat->stat);
    }
    if(FAILED(hres))
        return hres;

    if(push_instr(ctx, OP_pop_except) == -1)
        return E_OUTOFMEMORY;

    if(stat->catch_block) {
        unsigned jmp_finally;

        jmp_finally = push_instr(ctx, OP_jmp);
        if(jmp_finally == -1)
            return E_OUTOFMEMORY;

        instr_ptr(ctx, push_except)->arg1.uint = ctx->code_off;

        ctx->no_fallback = TRUE;
        hres = compile_statement(ctx, &catch_ctx, stat->catch_block->statement);
        ctx->no_fallback = prev_no_fallback;
        if(hres == E_NOTIMPL) {
            ctx->code_off = off_backup;
            stat->stat.eval = try_statement_eval;
            return compile_interp_fallback(ctx, &stat->stat);
        }
        if(FAILED(hres))
            return hres;

        if(push_instr(ctx, OP_pop_scope) == -1)
            return E_OUTOFMEMORY;

        instr_ptr(ctx, jmp_finally)->arg1.uint = ctx->code_off;
    }else {
        instr_ptr(ctx, push_except)->arg1.uint = ctx->code_off;
    }

    if(stat->finally_statement) {
        /* FIXME: avoid */
        if(push_instr(ctx, OP_pop) == -1)
            return E_OUTOFMEMORY;

        ctx->no_fallback = TRUE;
        hres = compile_statement(ctx, stat->catch_block ? NULL : &finally_ctx, stat->finally_statement);
        ctx->no_fallback = prev_no_fallback;
        if(hres == E_NOTIMPL) {
            ctx->code_off = off_backup;
            stat->stat.eval = try_statement_eval;
            return compile_interp_fallback(ctx, &stat->stat);
        }
        if(FAILED(hres))
            return hres;

        if(!stat->catch_block && push_instr(ctx, OP_end_finally) == -1)
            return E_OUTOFMEMORY;
    }

    return S_OK;
}

static HRESULT compile_statement(compiler_ctx_t *ctx, statement_ctx_t *stat_ctx, statement_t *stat)
{
    HRESULT hres;

    if(stat_ctx) {
        stat_ctx->next = ctx->stat_ctx;
        ctx->stat_ctx = stat_ctx;
    }

    switch(stat->type) {
    case STAT_BLOCK:
        hres = compile_block_statement(ctx, ((block_statement_t*)stat)->stat_list);
        break;
    case STAT_BREAK:
        hres = compile_break_statement(ctx, (branch_statement_t*)stat);
        break;
    case STAT_CONTINUE:
        hres = compile_continue_statement(ctx, (branch_statement_t*)stat);
        break;
    case STAT_EMPTY:
        hres = push_instr(ctx, OP_undefined) == -1 ? E_OUTOFMEMORY : S_OK; /* FIXME */
        break;
    case STAT_EXPR:
        hres = compile_expression_statement(ctx, (expression_statement_t*)stat);
        break;
    case STAT_FOR:
        hres = compile_for_statement(ctx, (for_statement_t*)stat);
        break;
    case STAT_FORIN:
        hres = compile_forin_statement(ctx, (forin_statement_t*)stat);
        break;
    case STAT_IF:
        hres = compile_if_statement(ctx, (if_statement_t*)stat);
        break;
    case STAT_LABEL:
        hres = push_instr(ctx, OP_label) == -1 ? E_OUTOFMEMORY : S_OK; /* FIXME */
        break;
    case STAT_SWITCH:
        hres = compile_switch_statement(ctx, (switch_statement_t*)stat);
        break;
    case STAT_THROW:
        hres = compile_throw_statement(ctx, (expression_statement_t*)stat);
        break;
    case STAT_TRY:
        hres = compile_try_statement(ctx, (try_statement_t*)stat);
        break;
    case STAT_VAR:
        hres = compile_var_statement(ctx, (var_statement_t*)stat);
        break;
    case STAT_WHILE:
        hres = compile_while_statement(ctx, (while_statement_t*)stat);
        break;
    case STAT_WITH:
        hres = compile_with_statement(ctx, (with_statement_t*)stat);
        break;
    default:
        hres = compile_interp_fallback(ctx, stat);
    }

    if(stat_ctx) {
        assert(ctx->stat_ctx == stat_ctx);
        ctx->stat_ctx = stat_ctx->next;
    }

    return hres;
}

static void resolve_labels(compiler_ctx_t *ctx, unsigned off)
{
    instr_t *instr;

    for(instr = ctx->code->instrs+off; instr < ctx->code->instrs+ctx->code_off; instr++) {
        if(instr_info[instr->op].arg1_type == ARG_ADDR && (instr->arg1.uint & LABEL_FLAG)) {
            assert((instr->arg1.uint & ~LABEL_FLAG) < ctx->labels_cnt);
            instr->arg1.uint = ctx->labels[instr->arg1.uint & ~LABEL_FLAG];
        }
        assert(instr_info[instr->op].arg2_type != ARG_ADDR);
    }

    ctx->labels_cnt = 0;
}

void release_bytecode(bytecode_t *code)
{
    unsigned i;

    for(i=0; i < code->bstr_cnt; i++)
        SysFreeString(code->bstr_pool[i]);

    jsheap_free(&code->heap);
    heap_free(code->bstr_pool);
    heap_free(code->instrs);
    heap_free(code);
}

void release_compiler(compiler_ctx_t *ctx)
{
    heap_free(ctx);
}

static HRESULT init_compiler(parser_ctx_t *parser)
{
    if(!parser->code) {
        parser->code = heap_alloc_zero(sizeof(bytecode_t));
        if(!parser->code)
            return E_OUTOFMEMORY;
        jsheap_init(&parser->code->heap);
    }

    if(!parser->compiler) {
        parser->compiler = heap_alloc_zero(sizeof(compiler_ctx_t));
        if(!parser->compiler)
            return E_OUTOFMEMORY;

        parser->compiler->parser = parser;
        parser->compiler->code = parser->code;
    }

    return S_OK;
}

HRESULT compile_subscript(parser_ctx_t *parser, expression_t *expr, unsigned *ret_off)
{
    HRESULT hres;

    hres = init_compiler(parser);
    if(FAILED(hres))
        return hres;

    *ret_off = parser->compiler->code_off;
    hres = compile_expression(parser->compiler, expr);
    if(FAILED(hres))
        return hres;

    return push_instr(parser->compiler, OP_ret) == -1 ? E_OUTOFMEMORY : S_OK;
}

HRESULT compile_subscript_stat(parser_ctx_t *parser, statement_t *stat, BOOL compile_block, unsigned *ret_off)
{
    HRESULT hres;

    TRACE("\n");

    hres = init_compiler(parser);
    if(FAILED(hres))
        return hres;

    *ret_off = parser->compiler->code_off;
    if(compile_block && stat->next)
        hres = compile_block_statement(parser->compiler, stat);
    else
        hres = compile_statement(parser->compiler, NULL, stat);
    if(FAILED(hres))
        return hres;

    resolve_labels(parser->compiler, *ret_off);

    return push_instr(parser->compiler, OP_ret) == -1 ? E_OUTOFMEMORY : S_OK;
}
