/*
 * Tiny-C microcoder (mljOS app)
 *
 * Minimal Tiny-C compiler + VM:
 * - Reads Tiny-C source from the user (through mljos_api_t)
 * - Compiles it to a small bytecode
 * - Executes it immediately in a VM
 * - Prints values of non-zero variables a..z
 */

#include "sdk/mljos_api.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

#define MAX_LINE_LEN 256
#define MAX_PROGRAM_LEN 4096

#define MAX_NODES 4096
#define MAX_CODE 1000
#define MAX_STACK 1000

// ----- Lexer -----
enum {
    DO_SYM,
    ELSE_SYM,
    IF_SYM,
    WHILE_SYM,
    LBRA,
    RBRA,
    LPAR,
    RPAR,
    PLUS,
    MINUS,
    LESS,
    SEMI,
    EQUAL,
    GREATER,
    MUL,
    DIV,
    MOD,
    GE,
    LE,
    EQ,
    NE,
    INT,
    ID,
    FOR_SYM,
    PRINT_SYM,
    EOI
};

static const char *g_src;
static int g_pos;
static int g_ch;
static int g_sym;
static int g_int_val;
static char g_id_name[32];

static int g_error;
static const char *g_error_msg;

// Prototypes
static int compile_and_run(mljos_api_t *api, const char *program_text);

void _start(mljos_api_t *api) {
    if (!api->open_path || !api->open_path[0]) {
        api->puts("Tiny-C microcoder for mljOS\n");
        api->puts("Usage: microcoder <file.tc>\n");
        return;
    }

    char program_text[MAX_PROGRAM_LEN];
    unsigned int size = 0;

    if (api->read_file(api->open_path, program_text, sizeof(program_text) - 1, &size)) {
        program_text[size] = '\0';
        compile_and_run(api, program_text);
    } else {
        api->puts("Tiny-C error: could not read file '");
        api->puts(api->open_path);
        api->puts("'\n");
    }
}

static int streq(const char *a, const char *b) {
    int i = 0;
    while (a && b && a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return (!a || !b) ? 0 : (a[i] == b[i]);
}

static void syntax_error(const char *msg) {
    g_error = 1;
    g_error_msg = msg ? msg : "syntax error";
    g_sym = EOI; // Stop parsing as soon as possible.
}

static void next_ch(void) {
    g_ch = g_src[g_pos];
    if (g_ch) g_pos++;
}

static void lexer_init(const char *src) {
    g_src = src;
    g_pos = 0;
    g_ch = ' ';
    g_sym = EOI;
    g_int_val = 0;
    g_id_name[0] = '\0';
    g_error = 0;
    g_error_msg = NULL;
    // Note: next_sym() will advance g_ch as needed.
}

static void next_sym(void) {
again:
    switch (g_ch) {
        case ' ':
        case '\n':
        case '\r':
        case '\t':
            next_ch();
            goto again;
        case 0:
            g_sym = EOI;
            return;
        case '{':
            next_ch();
            g_sym = LBRA;
            return;
        case '}':
            next_ch();
            g_sym = RBRA;
            return;
        case '(':
            next_ch();
            g_sym = LPAR;
            return;
        case ')':
            next_ch();
            g_sym = RPAR;
            return;
        case '+':
            next_ch();
            g_sym = PLUS;
            return;
        case '-':
            next_ch();
            g_sym = MINUS;
            return;
        case '=':
            next_ch();
            if (g_ch == '=') {
                next_ch();
                g_sym = EQ;
            } else {
                g_sym = EQUAL;
            }
            return;
        case '!':
            next_ch();
            if (g_ch == '=') {
                next_ch();
                g_sym = NE;
            } else {
                syntax_error("expected '=' after '!'");
            }
            return;
        case '>':
            next_ch();
            if (g_ch == '=') {
                next_ch();
                g_sym = GE;
            } else {
                g_sym = GREATER;
            }
            return;
        case '<':
            next_ch();
            if (g_ch == '=') {
                next_ch();
                g_sym = LE;
            } else {
                g_sym = LESS;
            }
            return;
        case '*':
            next_ch();
            g_sym = MUL;
            return;
        case '/':
            next_ch();
            if (g_ch == '/') {
                while (g_ch && g_ch != '\n') next_ch();
                goto again;
            }
            if (g_ch == '*') {
                next_ch();
                while (g_ch) {
                    if (g_ch == '*') {
                        next_ch();
                        if (g_ch == '/') {
                            next_ch();
                            goto again;
                        }
                    } else {
                        next_ch();
                    }
                }
                syntax_error("unclosed comment");
                return;
            }
            g_sym = DIV;
            return;
        case '%':
            next_ch();
            g_sym = MOD;
            return;
        case ';':
            next_ch();
            g_sym = SEMI;
            return;
        default:
            break;
    }

    if (g_error) return;

    if (g_ch >= '0' && g_ch <= '9') {
        int v = 0;
        while (g_ch >= '0' && g_ch <= '9') {
            v = v * 10 + (g_ch - '0');
            next_ch();
        }
        g_int_val = v;
        g_sym = INT;
        return;
    }

    if ((g_ch >= 'a' && g_ch <= 'z') || (g_ch >= 'A' && g_ch <= 'Z')) {
        int i = 0;
        while ((g_ch >= 'a' && g_ch <= 'z') || (g_ch >= 'A' && g_ch <= 'Z') || g_ch == '_') {
            char c = (char)g_ch;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (i < (int)sizeof(g_id_name) - 1) g_id_name[i++] = c;
            else syntax_error("identifier too long");
            next_ch();
            if (g_error) return;
        }
        g_id_name[i] = '\0';

        if (streq(g_id_name, "do")) {
            g_sym = DO_SYM;
            return;
        }
        if (streq(g_id_name, "else")) {
            g_sym = ELSE_SYM;
            return;
        }
        if (streq(g_id_name, "if")) {
            g_sym = IF_SYM;
            return;
        }
        if (streq(g_id_name, "while")) {
            g_sym = WHILE_SYM;
            return;
        }
        if (streq(g_id_name, "for")) {
            g_sym = FOR_SYM;
            return;
        }
        if (streq(g_id_name, "print")) {
            g_sym = PRINT_SYM;
            return;
        }

        // Variables are single letters a..z.
        if (i == 1 && g_id_name[0] >= 'a' && g_id_name[0] <= 'z') {
            g_sym = ID;
            return;
        }

        syntax_error("unknown identifier");
        return;
    }

    syntax_error("invalid token");
}

// ----- Parser + AST -----
enum {
    VAR,
    CST,
    ADD,
    SUB,
    LT,
    SET,
    IF1,
    IF2,
    WHILE,
    DO,
    FOR,
    PRINT,
    MUL_NODE,
    DIV_NODE,
    MOD_NODE,
    GT_NODE,
    GE_NODE,
    LE_NODE,
    EQ_NODE,
    NE_NODE,
    EMPTY,
    SEQ,
    EXPR,
    PROG
};

typedef struct node {
    int kind;
    struct node *o1;
    struct node *o2;
    struct node *o3;
    int val;
} node;

static node g_pool[MAX_NODES];
static int g_pool_used;

static node *new_node(int k) {
    if (g_error) return NULL;
    if (g_pool_used >= MAX_NODES) {
        syntax_error("program too complex");
        return NULL;
    }
    node *x = &g_pool[g_pool_used++];
    x->kind = k;
    x->o1 = x->o2 = x->o3 = 0;
    x->val = 0;
    return x;
}

static node *paren_expr(void);
static node *statement_block(void);

static node *term(void) {
    if (g_error) return NULL;
    if (g_sym == ID) {
        node *x = new_node(VAR);
        if (!x) return NULL;
        x->val = g_id_name[0] - 'a';
        next_sym();
        return x;
    }
    if (g_sym == INT) {
        node *x = new_node(CST);
        if (!x) return NULL;
        x->val = g_int_val;
        next_sym();
        return x;
    }
    return paren_expr();
}
    
static node *factor(void) {
    if (g_error) return NULL;
    node *x = term();
    while (!g_error && (g_sym == MUL || g_sym == DIV || g_sym == MOD)) {
        int k = (g_sym == MUL) ? MUL_NODE : (g_sym == DIV) ? DIV_NODE : MOD_NODE;
        next_sym();
        node *t = x;
        node *r = term();
        node *n = new_node(k);
        if (!n) return NULL;
        n->o1 = t;
        n->o2 = r;
        x = n;
    }
    return x;
}

static node *sum(void) {
    if (g_error) return NULL;
    node *x = factor();
    while (!g_error && (g_sym == PLUS || g_sym == MINUS)) {
        int k = (g_sym == PLUS) ? ADD : SUB;
        next_sym();
        node *t = x;
        node *r = term();
        node *n = new_node(k);
        if (!n) return NULL;
        n->o1 = t;
        n->o2 = r;
        x = n;
    }
    return x;
}

static node *test(void) {
    if (g_error) return NULL;
    node *x = sum();
    if (!g_error && (g_sym == LESS || g_sym == GREATER || g_sym == LE || g_sym == GE || g_sym == EQ || g_sym == NE)) {
        int k = (g_sym == LESS) ? LT : (g_sym == GREATER) ? GT_NODE : (g_sym == LE) ? LE_NODE : (g_sym == GE) ? GE_NODE : (g_sym == EQ) ? EQ_NODE : NE_NODE;
        next_sym();
        node *t = x;
        node *r = sum();
        node *n = new_node(k);
        if (!n) return NULL;
        n->o1 = t;
        n->o2 = r;
        x = n;
    }
    return x;
}

static node *expr(void) {
    if (g_error) return NULL;
    if (g_sym != ID) return test();

    node *x = test();
    if (!g_error && x && x->kind == VAR && g_sym == EQUAL) {
        next_sym();
        node *rhs = expr();
        node *n = new_node(SET);
        if (!n) return NULL;
        n->o1 = x;   // left var
        n->o2 = rhs; // right expr
        return n;
    }
    return x;
}

static node *paren_expr(void) {
    if (g_error) return NULL;
    if (g_sym != LPAR) {
        syntax_error("expected '('");
        return NULL;
    }
    next_sym();
    node *x = expr();
    if (g_error) return NULL;
    if (g_sym != RPAR) {
        syntax_error("expected ')'");
        return NULL;
    }
    next_sym();
    return x;
}

static node *statement(void) {
    if (g_error) return NULL;

    if (g_sym == IF_SYM) {
        node *x = new_node(IF1);
        if (!x) return NULL;
        next_sym();
        x->o1 = paren_expr();
        x->o2 = statement();
        if (!g_error && g_sym == ELSE_SYM) {
            x->kind = IF2;
            next_sym();
            x->o3 = statement();
        }
        return x;
    }

    if (g_sym == WHILE_SYM) {
        node *x = new_node(WHILE);
        if (!x) return NULL;
        next_sym();
        x->o1 = paren_expr();
        x->o2 = statement();
        return x;
    }

    if (g_sym == DO_SYM) {
        node *x = new_node(DO);
        if (!x) return NULL;
        next_sym();
        x->o1 = statement();
        if (g_sym != WHILE_SYM) {
            syntax_error("expected 'while' after do-statement");
            return NULL;
        }
        next_sym();
        x->o2 = paren_expr();
        if (g_sym != SEMI) {
            syntax_error("expected ';' after do-while");
            return NULL;
        }
        next_sym();
        return x;
    }

    if (g_sym == SEMI) {
        next_sym();
        return new_node(EMPTY);
    }

    if (g_sym == LBRA) {
        return statement_block();
    }

    if (g_sym == FOR_SYM) {
        node *x = new_node(FOR);
        if (!x) return NULL;
        next_sym();
        if (g_sym != LPAR) { syntax_error("expected '(' after for"); return NULL; }
        next_sym();
        if (g_sym != SEMI) x->o1 = expr();
        if (g_sym != SEMI) { syntax_error("expected ';' in for"); return NULL; }
        next_sym();
        if (g_sym != SEMI) x->o2 = expr();
        if (g_sym != SEMI) { syntax_error("expected ';' in for"); return NULL; }
        next_sym();
        if (g_sym != RPAR) x->o3 = expr();
        if (g_sym != RPAR) { syntax_error("expected ')' in for"); return NULL; }
        next_sym();
        node *stmt = statement();
        // Pack loop body and step into a sequence if needed, but easier to use another node.
        // Let's use x->val or o4? node only has o1, o2, o3.
        // Strategy for FOR: o1 = init, o2 = cond, o3 = step, o4 = body.
        // But node only has 3 pointers. Let's use a nested structure.
        node *body_node = new_node(SEQ);
        if (!body_node) return NULL;
        body_node->o1 = stmt;
        body_node->o2 = x->o3; // step
        x->o3 = body_node;
        return x;
    }

    if (g_sym == PRINT_SYM) {
        node *x = new_node(PRINT);
        if (!x) return NULL;
        next_sym();
        x->o1 = expr();
        if (g_sym != SEMI) { syntax_error("expected ';' after print"); return NULL; }
        next_sym();
        return x;
    }

    // <expr> ';'
    {
        node *x = new_node(EXPR);
        if (!x) return NULL;
        x->o1 = expr();
        if (g_error) return NULL;
        if (g_sym != SEMI) {
            // Check if it's just a semicolon (empty statement) which was handled above.
            syntax_error("expected ';' after expression");
            return NULL;
        }
        next_sym();
        return x;
    }
}

// Blocks need a separate implementation to avoid parser token juggling.
static node *statement_block(void) {
    // Current token must be '{'
    if (g_sym != LBRA) {
        syntax_error("expected '{'");
        return NULL;
    }
    next_sym(); // consume '{'
    node *x = new_node(EMPTY);
    if (!x) return NULL;

    while (!g_error && g_sym != RBRA) {
        node *t = x;
        node *s = statement();
        node *n = new_node(SEQ);
        if (!n) return NULL;
        n->o1 = t;
        n->o2 = s;
        x = n;
    }
    if (g_error) return NULL;
    if (g_sym != RBRA) {
        syntax_error("expected '}'");
        return NULL;
    }
    next_sym(); // consume '}'
    return x;
}

static node *program(void) {
    g_pool_used = 0;
    if (g_error) return NULL;
    node *x = new_node(PROG);
    if (!x) return NULL;
    
    next_sym();
    node *root = new_node(EMPTY);
    if (!root) return NULL;
    
    while (!g_error && g_sym != EOI) {
        node *stmt = statement();
        node *n = new_node(SEQ);
        if (!n) return NULL;
        n->o1 = root;
        n->o2 = stmt;
        root = n;
    }
    x->o1 = root;
    return x;
}

// ----- Code generator -----
enum {
    IFETCH,
    ISTORE,
    IPUSH,
    IPOP,
    IADD,
    ISUB,
    IMUL,
    IDIV,
    IMOD,
    ILT,
    IGT,
    IGE,
    ILE,
    IEQ,
    INE,
    JZ,
    JNZ,
    JMP,
    IPRINT,
    HALT
};

typedef signed char code;

static code g_object[MAX_CODE];
static code *g_here;

static void emit(code c) {
    if (g_error) return;
    if ((g_here - g_object) >= (int)MAX_CODE) syntax_error("bytecode overflow");
    else *g_here++ = c;
}

static code *hole(void) {
    if (g_error) return 0;
    if ((g_here - g_object) >= (int)MAX_CODE) syntax_error("bytecode overflow");
    else return g_here++;
    return 0;
}

static void fix(code *src, code *dst) {
    if (g_error) return;
    // src is the offset byte address. Jump is relative to the address AFTER the offset.
    int diff = (int)(dst - (src + 1));
    if (diff < -128 || diff > 127) syntax_error("jump offset overflow");
    else *src = (code)diff;
}

static void gen(node *x) {
    if (!x || g_error) return;

    code *p1, *p2;
    switch (x->kind) {
        case VAR:
            emit(IFETCH);
            emit((code)x->val);
            return;
        case CST:
            emit(IPUSH);
            emit((code)(x->val & 0xFF));
            emit((code)((x->val >> 8) & 0xFF));
            emit((code)((x->val >> 16) & 0xFF));
            emit((code)((x->val >> 24) & 0xFF));
            return;
        case ADD:
            gen(x->o1);
            gen(x->o2);
            emit(IADD);
            return;
        case SUB:
            gen(x->o1);
            gen(x->o2);
            emit(ISUB);
            return;
        case MUL_NODE:
            gen(x->o1);
            gen(x->o2);
            emit(IMUL);
            return;
        case DIV_NODE:
            gen(x->o1);
            gen(x->o2);
            emit(IDIV);
            return;
        case MOD_NODE:
            gen(x->o1);
            gen(x->o2);
            emit(IMOD);
            return;
        case LT:
            gen(x->o1);
            gen(x->o2);
            emit(ILT);
            return;
        case GT_NODE:
            gen(x->o1);
            gen(x->o2);
            emit(IGT);
            return;
        case GE_NODE:
            gen(x->o1);
            gen(x->o2);
            emit(IGE);
            return;
        case LE_NODE:
            gen(x->o1);
            gen(x->o2);
            emit(ILE);
            return;
        case EQ_NODE:
            gen(x->o1);
            gen(x->o2);
            emit(IEQ);
            return;
        case NE_NODE:
            gen(x->o1);
            gen(x->o2);
            emit(INE);
            return;
        case SET:
            gen(x->o2);
            emit(ISTORE);
            emit((code)x->o1->val);
            return;
        case IF1:
            gen(x->o1);
            emit(JZ);
            p1 = hole();
            gen(x->o2);
            fix(p1, g_here);
            return;
        case IF2:
            gen(x->o1);
            emit(JZ);
            p1 = hole();
            gen(x->o2);
            emit(JMP);
            p2 = hole();
            fix(p1, g_here);
            gen(x->o3);
            fix(p2, g_here);
            return;
        case WHILE:
            p1 = g_here;
            gen(x->o1);
            emit(JZ);
            p2 = hole();
            gen(x->o2);
            emit(JMP);
            fix(hole(), p1);
            fix(p2, g_here);
            return;
        case DO:
            p1 = g_here;
            gen(x->o1);
            gen(x->o2);
            emit(JNZ);
            fix(hole(), p1);
            return;
        case FOR:
            if (x->o1) {
                gen(x->o1);
                emit(IPOP);
            }
            p1 = g_here;
            p2 = NULL;
            if (x->o2) {
                gen(x->o2);
                emit(JZ);
                p2 = hole();
            }
            gen(x->o3->o1); // body
            if (x->o3->o2) {
                gen(x->o3->o2);
                emit(IPOP);
            }
            emit(JMP);
            fix(hole(), p1);
            if (p2) fix(p2, g_here);
            return;
        case PRINT:
            gen(x->o1);
            emit(IPRINT);
            return;
        case EMPTY:
            return;
        case SEQ:
            gen(x->o1);
            gen(x->o2);
            return;
        case EXPR:
            gen(x->o1);
            emit(IPOP);
            return;
        case PROG:
            gen(x->o1);
            emit(HALT);
            return;
    }
}

// ----- VM -----
static int g_globals[26];
static int g_vm_stack[MAX_STACK];

static void print_int(mljos_api_t *api, int v) {
    if (!api) return;
    if (v == 0) {
        api->putchar('0');
        return;
    }

    if (v < 0) {
        api->putchar('-');
        v = -v;
    }

    char buf[16];
    int n = 0;
    while (v > 0 && n < (int)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n-- > 0) api->putchar(buf[n]);
}

static void run_vm(mljos_api_t *api) {
    int *sp = g_vm_stack;
    code *pc = g_object;

    for (int i = 0; i < 26; i++) g_globals[i] = 0;

    again:
    switch (*pc++) {
        case IFETCH:
            *sp++ = g_globals[*pc++];
            goto again;
        case ISTORE:
            g_globals[*pc++] = sp[-1];
            goto again;
        case IPUSH: {
            int v = (unsigned char)(*pc++);
            v |= (unsigned char)(*pc++) << 8;
            v |= (unsigned char)(*pc++) << 16;
            v |= (unsigned char)(*pc++) << 24;
            *sp++ = v;
            goto again;
        }
        case IPOP:
            --sp;
            goto again;
        case IADD:
            sp[-2] = sp[-2] + sp[-1];
            --sp;
            goto again;
        case ISUB:
            sp[-2] = sp[-2] - sp[-1];
            --sp;
            goto again;
        case IMUL:
            sp[-2] = sp[-2] * sp[-1];
            --sp;
            goto again;
        case IDIV:
            if (sp[-1] != 0) sp[-2] = sp[-2] / sp[-1];
            else sp[-2] = 0;
            --sp;
            goto again;
        case IMOD:
            if (sp[-1] != 0) sp[-2] = sp[-2] % sp[-1];
            else sp[-2] = 0;
            --sp;
            goto again;
        case ILT:
            sp[-2] = (sp[-2] < sp[-1]);
            --sp;
            goto again;
        case IGT:
            sp[-2] = (sp[-2] > sp[-1]);
            --sp;
            goto again;
        case IGE:
            sp[-2] = (sp[-2] >= sp[-1]);
            --sp;
            goto again;
        case ILE:
            sp[-2] = (sp[-2] <= sp[-1]);
            --sp;
            goto again;
        case IEQ:
            sp[-2] = (sp[-2] == sp[-1]);
            --sp;
            goto again;
        case INE:
            sp[-2] = (sp[-2] != sp[-1]);
            --sp;
            goto again;
        case JMP:
            pc += *pc;
            goto again;
        case JZ:
            if (*--sp == 0) pc += *pc;
            else pc++;
            goto again;
        case JNZ:
            if (*--sp != 0) pc += *pc;
            else pc++;
            goto again;
        case IPRINT:
            print_int(api, *--sp);
            api->putchar('\n');
            goto again;
        case HALT:
            break;
        default:
            break;
    }

}

static int compile_and_run(mljos_api_t *api, const char *program_text) {
    g_error = 0;
    g_error_msg = NULL;
    lexer_init(program_text);

    if (g_error) {
        api->puts("Tiny-C error: ");
        api->puts(g_error_msg);
        api->putchar('\n');
        return 0;
    }

    // Build AST.
    g_pool_used = 0;
    node *root = program();
    if (g_error || !root) {
        api->puts("Tiny-C error: ");
        api->puts(g_error_msg ? g_error_msg : "syntax error");
        api->putchar('\n');
        return 0;
    }

    // Generate bytecode.
    g_here = g_object;
    gen(root);
    if (g_error) {
        api->puts("Tiny-C error: ");
        api->puts(g_error_msg ? g_error_msg : "codegen error");
        api->putchar('\n');
        return 0;
    }

    // Execute.
    run_vm(api);
    return 1;
}


