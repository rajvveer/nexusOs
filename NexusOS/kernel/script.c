/* ============================================================================
 * NexusOS — Scripting Engine (Implementation) — Phase 36
 * ============================================================================
 * NexusScript interpreter. Pipeline:
 *
 *   source text --[lex]--> token array --[interpret]--> effects
 *
 * The interpreter is a recursive-descent evaluator that walks the token array
 * with a cursor (it->pos). Control-flow keywords (if/while) record/skip token
 * positions rather than building a separate AST, which keeps the whole engine
 * compact and heap-light. Two helpers do the structural work:
 *   exec_block() — run statements until a block terminator (end/elif/else/EOF),
 *                  leaving the cursor ON that terminator.
 *   skip_block() — skip a block body (tracking nested if/while) without
 *                  executing, leaving the cursor on its terminator.
 * ============================================================================ */

#include "script.h"
#include "vga.h"
#include "string.h"
#include "heap.h"
#include "vfs.h"
#include "ramfs.h"
#include "shell.h"   /* shell_exec_line() for the `run` statement */

/* ============================================================================
 * Tokens
 * ============================================================================ */

enum {
    T_EOF = 0, T_NL,
    T_NUM, T_STR, T_IDENT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT,
    T_LP, T_RP,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE
};

typedef struct {
    uint8_t type;
    int     num;                     /* for T_NUM                            */
    int     line;                    /* 1-based source line                  */
    char    text[SCRIPT_TOK_TEXT];   /* for T_STR / T_IDENT                  */
} token_t;

/* ============================================================================
 * Runtime values & variables
 * ============================================================================ */

typedef struct {
    bool is_str;
    int  i;
    char s[SCRIPT_STR_MAX];
} value_t;

typedef struct {
    bool used;
    char name[SCRIPT_TOK_TEXT];
    value_t val;
} var_t;

typedef struct {
    token_t* toks;
    int      ntok;
    int      pos;
    var_t    vars[SCRIPT_MAX_VARS];
    bool     error;
    char     errmsg[80];
    int      errline;
    uint32_t steps;
    int      depth;
} interp_t;

static int script_nest = 0;          /* guards `run "script ..."` recursion  */

/* ============================================================================
 * Small helpers
 * ============================================================================ */

static value_t val_int(int n) { value_t v; v.is_str = false; v.i = n; v.s[0] = '\0'; return v; }

static value_t val_str(const char* s) {
    value_t v; v.is_str = true; v.i = 0;
    strncpy(v.s, s, SCRIPT_STR_MAX - 1);
    v.s[SCRIPT_STR_MAX - 1] = '\0';
    return v;
}

/* Render a value to text (string passes through, int -> decimal). */
static void val_to_str(const value_t* v, char* out) {
    if (v->is_str) {
        strncpy(out, v->s, SCRIPT_STR_MAX - 1);
        out[SCRIPT_STR_MAX - 1] = '\0';
    } else {
        int_to_str(v->i, out);
    }
}

static bool val_truthy(const value_t* v) {
    return v->is_str ? (v->s[0] != '\0') : (v->i != 0);
}

static void set_error(interp_t* it, const char* msg) {
    if (it->error) return;
    it->error = true;
    strncpy(it->errmsg, msg, sizeof(it->errmsg) - 1);
    it->errmsg[sizeof(it->errmsg) - 1] = '\0';
    it->errline = (it->pos < it->ntok) ? it->toks[it->pos].line : 0;
}

static token_t* cur(interp_t* it) {
    return &it->toks[it->pos < it->ntok ? it->pos : it->ntok]; /* ntok = EOF */
}

static token_t* peek(interp_t* it, int k) {
    int p = it->pos + k;
    if (p > it->ntok) p = it->ntok;
    return &it->toks[p];
}

static void advance(interp_t* it) {
    if (it->toks[it->pos < it->ntok ? it->pos : it->ntok].type != T_EOF)
        it->pos++;
}

static bool tok_is_kw(const token_t* t, const char* kw) {
    return t->type == T_IDENT && strcmp(t->text, kw) == 0;
}

static bool is_reserved(const char* s) {
    static const char* kws[] = {
        "let", "print", "run", "if", "elif", "else", "end",
        "while", "do", "then", "and", "or", "not", "true", "false", NULL
    };
    for (int i = 0; kws[i]; i++)
        if (strcmp(s, kws[i]) == 0) return true;
    return false;
}

static bool is_block_terminator(interp_t* it) {
    token_t* t = cur(it);
    return tok_is_kw(t, "end") || tok_is_kw(t, "elif") || tok_is_kw(t, "else");
}

/* ============================================================================
 * Lexer
 * ============================================================================ */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

/* Tokenize `src` into `toks` (capacity SCRIPT_MAX_TOKENS, last slot reserved
 * for the EOF sentinel). Returns token count, or -1 on lex error/overflow. */
static int lex(const char* src, token_t* toks) {
    int n = 0;
    int line = 1;
    const char* p = src;

    #define PUSH(tp) do {                                  \
        if (n >= SCRIPT_MAX_TOKENS - 1) return -1;         \
        toks[n].type = (tp); toks[n].num = 0;              \
        toks[n].line = line; toks[n].text[0] = '\0';       \
    } while (0)

    while (*p) {
        char c = *p;

        if (c == '\n') { PUSH(T_NL); n++; p++; line++; continue; }
        if (c == '\r' || c == ' ' || c == '\t') { p++; continue; }
        if (c == ';') { PUSH(T_NL); n++; p++; continue; }   /* statement sep */
        if (c == '#') { while (*p && *p != '\n') p++; continue; } /* comment  */

        /* Number */
        if (is_digit(c)) {
            int v = 0;
            while (is_digit(*p)) { v = v * 10 + (*p - '0'); p++; }
            PUSH(T_NUM); toks[n].num = v; n++;
            continue;
        }

        /* Identifier / keyword */
        if (is_alpha(c)) {
            int len = 0;
            PUSH(T_IDENT);
            while (is_alnum(*p) && len < SCRIPT_TOK_TEXT - 1) {
                toks[n].text[len++] = *p; p++;
            }
            toks[n].text[len] = '\0';
            while (is_alnum(*p)) p++;     /* skip overlong tail */
            n++;
            continue;
        }

        /* String literal with basic escapes */
        if (c == '"') {
            int len = 0;
            PUSH(T_STR);
            p++; /* opening quote */
            while (*p && *p != '"') {
                char ch = *p;
                if (ch == '\\' && p[1]) {
                    p++;
                    switch (*p) {
                        case 'n': ch = '\n'; break;
                        case 't': ch = '\t'; break;
                        case '"': ch = '"';  break;
                        case '\\': ch = '\\'; break;
                        default:  ch = *p;   break;
                    }
                }
                if (ch == '\n') line++;
                if (len < SCRIPT_TOK_TEXT - 1) toks[n].text[len++] = ch;
                p++;
            }
            if (*p != '"') return -1;     /* unterminated string */
            p++; /* closing quote */
            toks[n].text[len] = '\0';
            n++;
            continue;
        }

        /* Two-char operators */
        if (c == '=' && p[1] == '=') { PUSH(T_EQ); n++; p += 2; continue; }
        if (c == '!' && p[1] == '=') { PUSH(T_NE); n++; p += 2; continue; }
        if (c == '<' && p[1] == '=') { PUSH(T_LE); n++; p += 2; continue; }
        if (c == '>' && p[1] == '=') { PUSH(T_GE); n++; p += 2; continue; }

        /* Single-char operators */
        switch (c) {
            case '+': PUSH(T_PLUS);   n++; p++; continue;
            case '-': PUSH(T_MINUS);  n++; p++; continue;
            case '*': PUSH(T_STAR);   n++; p++; continue;
            case '/': PUSH(T_SLASH);  n++; p++; continue;
            case '%': PUSH(T_PCT);    n++; p++; continue;
            case '(': PUSH(T_LP);     n++; p++; continue;
            case ')': PUSH(T_RP);     n++; p++; continue;
            case '=': PUSH(T_ASSIGN); n++; p++; continue;
            case '<': PUSH(T_LT);     n++; p++; continue;
            case '>': PUSH(T_GT);     n++; p++; continue;
            default: return -1;       /* unknown character */
        }
    }

    /* EOF sentinel */
    toks[n].type = T_EOF; toks[n].num = 0; toks[n].line = line; toks[n].text[0] = '\0';
    #undef PUSH
    return n;
}

/* ============================================================================
 * Variables
 * ============================================================================ */

static value_t* var_find(interp_t* it, const char* name) {
    for (int i = 0; i < SCRIPT_MAX_VARS; i++)
        if (it->vars[i].used && strcmp(it->vars[i].name, name) == 0)
            return &it->vars[i].val;
    return NULL;
}

static void var_set(interp_t* it, const char* name, value_t v) {
    value_t* existing = var_find(it, name);
    if (existing) { *existing = v; return; }
    for (int i = 0; i < SCRIPT_MAX_VARS; i++) {
        if (!it->vars[i].used) {
            it->vars[i].used = true;
            strncpy(it->vars[i].name, name, SCRIPT_TOK_TEXT - 1);
            it->vars[i].name[SCRIPT_TOK_TEXT - 1] = '\0';
            it->vars[i].val = v;
            return;
        }
    }
    set_error(it, "too many variables");
}

/* ============================================================================
 * Expression evaluator (recursive descent, by precedence)
 *   or > and > not > comparison > add/sub > mul/div/mod > unary > primary
 * ============================================================================ */

static value_t parse_expr(interp_t* it);

static value_t parse_primary(interp_t* it) {
    if (it->error) return val_int(0);
    if (++it->depth > SCRIPT_MAX_DEPTH) { set_error(it, "expression too deep"); return val_int(0); }

    value_t result = val_int(0);
    token_t* t = cur(it);

    switch (t->type) {
        case T_NUM:
            result = val_int(t->num);
            advance(it);
            break;

        case T_STR:
            result = val_str(t->text);
            advance(it);
            break;

        case T_LP:
            advance(it);
            result = parse_expr(it);
            if (cur(it)->type != T_RP) set_error(it, "expected ')'");
            else advance(it);
            break;

        case T_IDENT:
            if (tok_is_kw(t, "true"))  { advance(it); result = val_int(1); break; }
            if (tok_is_kw(t, "false")) { advance(it); result = val_int(0); break; }

            /* builtin call:  len(...) / str(...) / abs(...) */
            if ((strcmp(t->text, "len") == 0 || strcmp(t->text, "str") == 0 ||
                 strcmp(t->text, "abs") == 0) && peek(it, 1)->type == T_LP) {
                char fn[8];
                strncpy(fn, t->text, sizeof(fn) - 1); fn[sizeof(fn) - 1] = '\0';
                advance(it);  /* name */
                advance(it);  /* '(' */
                value_t arg = parse_expr(it);
                if (cur(it)->type != T_RP) { set_error(it, "expected ')'"); break; }
                advance(it);
                if (strcmp(fn, "len") == 0) {
                    char buf[SCRIPT_STR_MAX];
                    val_to_str(&arg, buf);
                    result = val_int((int)strlen(buf));
                } else if (strcmp(fn, "str") == 0) {
                    char buf[SCRIPT_STR_MAX];
                    val_to_str(&arg, buf);
                    result = val_str(buf);
                } else { /* abs */
                    if (arg.is_str) set_error(it, "abs() needs a number");
                    else result = val_int(arg.i < 0 ? -arg.i : arg.i);
                }
                break;
            }

            if (is_reserved(t->text)) { set_error(it, "unexpected keyword in expression"); break; }

            /* variable reference */
            {
                value_t* v = var_find(it, t->text);
                if (!v) { set_error(it, "undefined variable"); break; }
                result = *v;
                advance(it);
            }
            break;

        default:
            set_error(it, "unexpected token in expression");
            break;
    }

    it->depth--;
    return result;
}

static value_t parse_unary(interp_t* it) {
    if (cur(it)->type == T_MINUS) {
        advance(it);
        value_t v = parse_unary(it);
        if (v.is_str) { set_error(it, "cannot negate a string"); return val_int(0); }
        return val_int(-v.i);
    }
    return parse_primary(it);
}

static value_t parse_mul(interp_t* it) {
    value_t left = parse_unary(it);
    while (!it->error) {
        uint8_t op = cur(it)->type;
        if (op != T_STAR && op != T_SLASH && op != T_PCT) break;
        advance(it);
        value_t right = parse_unary(it);
        if (left.is_str || right.is_str) { set_error(it, "arithmetic needs numbers"); break; }
        if ((op == T_SLASH || op == T_PCT) && right.i == 0) { set_error(it, "division by zero"); break; }
        if      (op == T_STAR)  left = val_int(left.i * right.i);
        else if (op == T_SLASH) left = val_int(left.i / right.i);
        else                    left = val_int(left.i % right.i);
    }
    return left;
}

static value_t parse_add(interp_t* it) {
    value_t left = parse_mul(it);
    while (!it->error) {
        uint8_t op = cur(it)->type;
        if (op != T_PLUS && op != T_MINUS) break;
        advance(it);
        value_t right = parse_mul(it);
        if (op == T_PLUS && (left.is_str || right.is_str)) {
            /* string concatenation */
            char a[SCRIPT_STR_MAX], b[SCRIPT_STR_MAX], out[SCRIPT_STR_MAX];
            val_to_str(&left, a);
            val_to_str(&right, b);
            int li = 0, k = 0;
            while (a[li] && k < SCRIPT_STR_MAX - 1) out[k++] = a[li++];
            int ri = 0;
            while (b[ri] && k < SCRIPT_STR_MAX - 1) out[k++] = b[ri++];
            out[k] = '\0';
            left = val_str(out);
        } else if (left.is_str || right.is_str) {
            set_error(it, "arithmetic needs numbers");
            break;
        } else {
            left = val_int(op == T_PLUS ? left.i + right.i : left.i - right.i);
        }
    }
    return left;
}

static value_t parse_cmp(interp_t* it) {
    value_t left = parse_add(it);
    while (!it->error) {
        uint8_t op = cur(it)->type;
        if (op != T_EQ && op != T_NE && op != T_LT &&
            op != T_LE && op != T_GT && op != T_GE) break;
        advance(it);
        value_t right = parse_add(it);

        bool eq;
        if (left.is_str || right.is_str) {
            char a[SCRIPT_STR_MAX], b[SCRIPT_STR_MAX];
            val_to_str(&left, a); val_to_str(&right, b);
            eq = (strcmp(a, b) == 0);
        } else {
            eq = (left.i == right.i);
        }

        if (op == T_EQ)      { left = val_int(eq ? 1 : 0); continue; }
        if (op == T_NE)      { left = val_int(eq ? 0 : 1); continue; }

        /* ordering comparisons require numbers */
        if (left.is_str || right.is_str) { set_error(it, "cannot order strings"); break; }
        int r;
        switch (op) {
            case T_LT: r = left.i <  right.i; break;
            case T_LE: r = left.i <= right.i; break;
            case T_GT: r = left.i >  right.i; break;
            default:   r = left.i >= right.i; break; /* T_GE */
        }
        left = val_int(r ? 1 : 0);
    }
    return left;
}

static value_t parse_not(interp_t* it) {
    if (tok_is_kw(cur(it), "not")) {
        advance(it);
        value_t v = parse_not(it);
        return val_int(val_truthy(&v) ? 0 : 1);
    }
    return parse_cmp(it);
}

static value_t parse_and(interp_t* it) {
    value_t left = parse_not(it);
    while (!it->error && tok_is_kw(cur(it), "and")) {
        advance(it);
        value_t right = parse_not(it);
        left = val_int((val_truthy(&left) && val_truthy(&right)) ? 1 : 0);
    }
    return left;
}

static value_t parse_or(interp_t* it) {
    value_t left = parse_and(it);
    while (!it->error && tok_is_kw(cur(it), "or")) {
        advance(it);
        value_t right = parse_and(it);
        left = val_int((val_truthy(&left) || val_truthy(&right)) ? 1 : 0);
    }
    return left;
}

static value_t parse_expr(interp_t* it) { return parse_or(it); }

/* ============================================================================
 * Statement execution
 * ============================================================================ */

static void exec_block(interp_t* it);
static void skip_block(interp_t* it);

static void skip_newlines(interp_t* it) {
    while (cur(it)->type == T_NL) advance(it);
}

/* Skip from an `if`/`elif` condition up to (and consuming) the `then`. */
static void skip_to_then(interp_t* it) {
    while (!it->error && cur(it)->type != T_EOF && !tok_is_kw(cur(it), "then")) {
        if (cur(it)->type == T_NL) { set_error(it, "expected 'then'"); return; }
        advance(it);
    }
    if (tok_is_kw(cur(it), "then")) advance(it);
    else set_error(it, "expected 'then'");
}

static void exec_if(interp_t* it) {
    advance(it); /* consume 'if' */
    bool handled = false;

    for (;;) {
        if (it->error) return;

        /* Evaluate (or skip) this branch's condition. */
        bool take;
        if (handled) {
            skip_to_then(it);
            take = false;
        } else {
            value_t cond = parse_expr(it);
            if (it->error) return;
            if (!tok_is_kw(cur(it), "then")) { set_error(it, "expected 'then'"); return; }
            advance(it);
            take = val_truthy(&cond);
        }

        if (take) { handled = true; exec_block(it); }
        else      { skip_block(it); }
        if (it->error) return;

        if (tok_is_kw(cur(it), "elif")) { advance(it); continue; }

        if (tok_is_kw(cur(it), "else")) {
            advance(it);
            if (!handled) exec_block(it);
            else          skip_block(it);
            if (it->error) return;
        }

        if (tok_is_kw(cur(it), "end")) { advance(it); return; }
        set_error(it, "expected 'end'");
        return;
    }
}

static void exec_while(interp_t* it) {
    int cond_pos = it->pos + 1; /* token after 'while' */

    for (;;) {
        if (it->error) return;
        if (++it->steps > SCRIPT_MAX_STEPS) { set_error(it, "step budget exceeded"); return; }

        it->pos = cond_pos;
        value_t cond = parse_expr(it);
        if (it->error) return;
        if (!tok_is_kw(cur(it), "do")) { set_error(it, "expected 'do'"); return; }
        advance(it);

        if (val_truthy(&cond)) {
            exec_block(it);              /* leaves cursor on 'end' */
            if (it->error) return;
            if (!tok_is_kw(cur(it), "end")) { set_error(it, "expected 'end'"); return; }
            /* loop back to re-evaluate the condition */
        } else {
            skip_block(it);              /* leaves cursor on 'end' */
            if (it->error) return;
            if (!tok_is_kw(cur(it), "end")) { set_error(it, "expected 'end'"); return; }
            advance(it);                 /* consume 'end' */
            return;
        }
    }
}

static void exec_statement(interp_t* it) {
    if (it->error) return;
    if (++it->steps > SCRIPT_MAX_STEPS) { set_error(it, "step budget exceeded"); return; }

    token_t* t = cur(it);

    if (t->type == T_EOF) return;

    /* Control flow */
    if (tok_is_kw(t, "if"))    { exec_if(it);    return; }
    if (tok_is_kw(t, "while")) { exec_while(it); return; }

    if (t->type == T_IDENT) {
        if (tok_is_kw(t, "let")) {
            advance(it);
            token_t* name = cur(it);
            if (name->type != T_IDENT || is_reserved(name->text)) {
                set_error(it, "expected variable name after 'let'"); return;
            }
            char vname[SCRIPT_TOK_TEXT];
            strncpy(vname, name->text, SCRIPT_TOK_TEXT - 1); vname[SCRIPT_TOK_TEXT - 1] = '\0';
            advance(it);
            if (cur(it)->type != T_ASSIGN) { set_error(it, "expected '=' in let"); return; }
            advance(it);
            value_t v = parse_expr(it);
            if (!it->error) var_set(it, vname, v);
        }
        else if (tok_is_kw(t, "print")) {
            advance(it);
            char buf[SCRIPT_STR_MAX];
            if (cur(it)->type == T_NL || cur(it)->type == T_EOF) {
                vga_print("\n");
            } else {
                value_t v = parse_expr(it);
                if (it->error) return;
                val_to_str(&v, buf);
                vga_print(buf);
                vga_print("\n");
            }
        }
        else if (tok_is_kw(t, "run")) {
            advance(it);
            value_t v = parse_expr(it);
            if (it->error) return;
            char buf[SCRIPT_STR_MAX];
            val_to_str(&v, buf);
            shell_exec_line(buf);
        }
        else if (is_reserved(t->text)) {
            set_error(it, "unexpected keyword");
            return;
        }
        else {
            /* reassignment:  name = expr */
            char vname[SCRIPT_TOK_TEXT];
            strncpy(vname, t->text, SCRIPT_TOK_TEXT - 1); vname[SCRIPT_TOK_TEXT - 1] = '\0';
            advance(it);
            if (cur(it)->type != T_ASSIGN) { set_error(it, "expected '=' after name"); return; }
            advance(it);
            value_t v = parse_expr(it);
            if (!it->error) var_set(it, vname, v);
        }
    } else {
        set_error(it, "expected a statement");
        return;
    }

    /* A simple statement must end at a newline / EOF / block terminator. */
    if (!it->error) {
        token_t* e = cur(it);
        if (e->type != T_NL && e->type != T_EOF && !is_block_terminator(it))
            set_error(it, "unexpected token after statement");
    }
}

/* Run statements until a block terminator or EOF; cursor left ON terminator. */
static void exec_block(interp_t* it) {
    for (;;) {
        if (it->error) return;
        skip_newlines(it);
        if (cur(it)->type == T_EOF) return;
        if (is_block_terminator(it)) return;
        exec_statement(it);
    }
}

/* Skip a block body, honoring nested if/while, without executing.
 * Cursor left ON the matching terminator (end/elif/else) or EOF. */
static void skip_block(interp_t* it) {
    int depth = 0;
    for (;;) {
        token_t* t = cur(it);
        if (t->type == T_EOF) return;
        if (t->type == T_IDENT) {
            if (tok_is_kw(t, "if") || tok_is_kw(t, "while")) { depth++; advance(it); continue; }
            if (tok_is_kw(t, "end")) {
                if (depth == 0) return;
                depth--; advance(it); continue;
            }
            if (depth == 0 && (tok_is_kw(t, "elif") || tok_is_kw(t, "else"))) return;
        }
        advance(it);
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char* script_strerror(int code) {
    switch (code) {
        case SCRIPT_OK:           return "ok";
        case SCRIPT_ERR_NOTFOUND: return "script file not found";
        case SCRIPT_ERR_TOOBIG:   return "script too large";
        case SCRIPT_ERR_NOMEM:    return "out of memory";
        case SCRIPT_ERR_SYNTAX:   return "syntax error";
        case SCRIPT_ERR_RUNTIME:  return "runtime error";
        case SCRIPT_ERR_NEST:     return "script nesting too deep";
        default:                  return "unknown error";
    }
}

int script_run_source(const char* src, const char* name) {
    if (script_nest >= SCRIPT_MAX_NEST) return SCRIPT_ERR_NEST;

    uint32_t srclen = (uint32_t)strlen(src);
    if (srclen >= SCRIPT_MAX_SRC) return SCRIPT_ERR_TOOBIG;

    interp_t* it = (interp_t*)kmalloc(sizeof(interp_t));
    token_t*  toks = (token_t*)kmalloc(sizeof(token_t) * SCRIPT_MAX_TOKENS);
    if (!it || !toks) {
        if (it) kfree(it);
        if (toks) kfree(toks);
        return SCRIPT_ERR_NOMEM;
    }

    memset(it, 0, sizeof(interp_t));
    it->toks = toks;

    int ntok = lex(src, toks);
    if (ntok < 0) {
        kfree(toks); kfree(it);
        vga_print_color("  [script] syntax error: bad token (script too large or malformed)\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return SCRIPT_ERR_SYNTAX;
    }
    it->ntok = ntok;

    script_nest++;
    exec_block(it);
    script_nest--;

    int rc = SCRIPT_OK;
    if (it->error) {
        vga_print_color("  [script] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        if (name) { vga_print((char*)name); }
        vga_print(" line ");
        char nb[12]; int_to_str(it->errline, nb); vga_print(nb);
        vga_print(": ");
        vga_print(it->errmsg);
        vga_print("\n");
        rc = SCRIPT_ERR_SYNTAX;
    }

    kfree(toks);
    kfree(it);
    return rc;
}

int script_run_file(const char* path) {
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, path);
    if (!node || (node->type & FS_DIRECTORY)) return SCRIPT_ERR_NOTFOUND;

    uint32_t sz = node->size;
    if (sz >= SCRIPT_MAX_SRC) return SCRIPT_ERR_TOOBIG;

    char* buf = (char*)kmalloc(sz + 1);
    if (!buf) return SCRIPT_ERR_NOMEM;

    int32_t rd = vfs_read(node, 0, sz, (uint8_t*)buf);
    if (rd < 0) { kfree(buf); return SCRIPT_ERR_NOTFOUND; }
    buf[rd] = '\0';

    int rc = script_run_source(buf, path);
    kfree(buf);
    return rc;
}

/* Sample script installed at boot so users have something to run immediately. */
static const char* SAMPLE_SCRIPT =
    "# NexusScript demo - try: script demo.ns\n"
    "print \"=== NexusScript demo ===\"\n"
    "let a = 7\n"
    "let b = 6\n"
    "print \"7 * 6 = \" + str(a * b)\n"
    "let i = 1\n"
    "while i <= 3 do\n"
    "  print \"count \" + str(i)\n"
    "  i = i + 1\n"
    "end\n"
    "if a * b == 42 then\n"
    "  print \"math checks out\"\n"
    "else\n"
    "  print \"impossible\"\n"
    "end\n"
    "print \"files on disk:\"\n"
    "run \"ls\"\n";

void script_init(void) {
    script_nest = 0;

    /* Install the sample script into the RAM filesystem. */
    fs_node_t* root = vfs_get_root();
    if (root && !vfs_finddir(root, "demo.ns")) {
        fs_node_t* node = ramfs_create("demo.ns", FS_FILE);
        if (node)
            vfs_write(node, 0, (uint32_t)strlen(SAMPLE_SCRIPT), (const uint8_t*)SAMPLE_SCRIPT);
    }

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Scripting engine ready (NexusScript, run 'script demo.ns')\n");
}
