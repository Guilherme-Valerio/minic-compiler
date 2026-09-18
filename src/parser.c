/*
 * parser.c - Analisador sintatico (parser) da linguagem MINIC, em C.
 *
 * Implementa exatamente a mesma gramatica, a mesma AST, as mesmas
 * mensagens de erro e a mesma recuperacao do parser em Python
 * (src/parser.py): as duas versoes produzem a mesma saida, byte a byte
 * (ver docs/especificacao-sintatica.md, secao 7).
 *
 * Uso:
 *   ./parser codigo.c              AST compacta (uma linha) no stdout
 *   ./parser codigo.c --arvore     AST indentada, um no por linha
 *   ./parser codigo.c --tokens     mostra tambem os tokens do scanner
 *
 * Codigo de saida: 0 = entrada aceita (AST impressa); 1 = entrada
 * rejeitada (diagnosticos no stderr, nenhuma AST); 2 = falha de execucao.
 *
 * Compilacao (arquivo unico, como no script de testes do professor):
 *   gcc -Wall -Wextra -std=c11 parser.c -o parser
 *
 * Integracao com o scanner (Etapa 1 -> Etapa 2): o src/scanner.c da
 * Etapa 1 e incluido sem nenhuma alteracao (#include "scanner.c"). O
 * parser roda o scanner, captura a saida JSON Lines que ele produz (a
 * mesma que ./scanner imprime no terminal) e le os tokens dessa saida.
 * Por isso scanner.c e parser.c precisam estar no mesmo diretorio.
 */

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== */
/* Utilidades: memoria e texto dinamico                                   */
/* ===================================================================== */

static void **g_allocs = NULL;
static size_t g_nallocs = 0, g_capallocs = 0;

static void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "memoria insuficiente\n"); exit(2); }
    if (g_nallocs == g_capallocs) {
        g_capallocs = g_capallocs ? g_capallocs * 2 : 256;
        void **np = realloc(g_allocs, g_capallocs * sizeof(void *));
        if (!np) { fprintf(stderr, "memoria insuficiente\n"); exit(2); }
        g_allocs = np;
    }
    g_allocs[g_nallocs++] = p;
    return p;
}

static void free_all(void) {
    for (size_t i = 0; i < g_nallocs; i++) free(g_allocs[i]);
    free(g_allocs);
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* vetor dinamico de ponteiros */
typedef struct {
    void **items;
    int n, cap;
} PtrList;

static void list_push(PtrList *l, void *p) {
    if (l->n == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 8;
        void **ni = xmalloc((size_t)ncap * sizeof(void *));
        if (l->n) memcpy(ni, l->items, (size_t)l->n * sizeof(void *));
        l->items = ni;
        l->cap = ncap;
    }
    l->items[l->n++] = p;
}

/* texto dinamico */
typedef struct {
    char *s;
    size_t len, cap;
} StrBuf;

static void sb_add(StrBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return;
    if (b->len + (size_t)need + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 256;
        while (b->len + (size_t)need + 1 > ncap) ncap *= 2;
        char *ns = xmalloc(ncap);
        if (b->len) memcpy(ns, b->s, b->len);
        b->s = ns;
        b->cap = ncap;
    }
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
}

/* ===================================================================== */
/* INTEGRACAO COM O SCANNER DA ETAPA 1 (src/scanner.c, sem alteracoes)    */
/* ===================================================================== */
/*
 * O scanner.c da Etapa 1 e incluido aqui como esta. Ele escreve cada token
 * como uma linha JSON no stdout e cada erro lexico como uma linha JSON no
 * stderr, sempre por meio de printf/fprintf/fputs/fputc. Antes do #include,
 * essas quatro funcoes sao redirecionadas (so dentro do scanner.c) para
 * buffers em memoria; o main() do scanner e renomeado para nao conflitar
 * com o main() do parser. Depois de rodar o scanner, o parser le as linhas
 * JSON capturadas - exatamente a saida da Etapa 1 - e as converte no vetor
 * de tokens que a analise sintatica consome.
 */

typedef struct {
    const char *type;   /* INT, IDENT, SEMICOLON, ..., EOF */
    char *lexeme;
    int line, column;
} Token;

typedef struct {
    const char *error;  /* UNKNOWN_SYMBOL, ... */
    char *lexeme;
    int line, column;
} LexError;

static PtrList g_tokens;    /* Token*    */
static PtrList g_lexerrs;   /* LexError* */

static StrBuf g_scan_stdout; /* linhas JSON dos tokens (stdout do scanner) */
static StrBuf g_scan_stderr; /* linhas JSON dos erros (stderr do scanner)  */

static StrBuf *scan_capture(FILE *f) {
    if (f == stdout) return &g_scan_stdout;
    if (f == stderr) return &g_scan_stderr;
    return NULL;
}

static int scan_hook_vfprintf(FILE *f, const char *fmt, va_list ap) {
    StrBuf *b = scan_capture(f);
    if (!b) return vfprintf(f, fmt, ap);
    char small[256];
    va_list copy;
    va_copy(copy, ap);
    int need = vsnprintf(small, sizeof(small), fmt, copy);
    va_end(copy);
    if (need < 0) return need;
    if ((size_t)need < sizeof(small)) {
        sb_add(b, "%s", small);
    } else {
        char *big = xmalloc((size_t)need + 1);
        vsnprintf(big, (size_t)need + 1, fmt, ap);
        sb_add(b, "%s", big);
    }
    return need;
}

static int scan_hook_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = scan_hook_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

static int scan_hook_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = scan_hook_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

static int scan_hook_fputs(const char *s, FILE *f) {
    StrBuf *b = scan_capture(f);
    if (!b) return fputs(s, f);
    sb_add(b, "%s", s);
    return 0;
}

static int scan_hook_fputc(int c, FILE *f) {
    StrBuf *b = scan_capture(f);
    if (!b) return fputc(c, f);
    if (c == 0) return c; /* bytes NUL nao ocorrem na saida JSON do scanner */
    sb_add(b, "%c", c);
    return c;
}

/* #undef antes de cada #define: algumas bibliotecas C definem essas
 * funcoes tambem como macros */
#undef printf
#undef fprintf
#undef fputs
#undef fputc
#define main    minic_scanner_main
#define printf  scan_hook_printf
#define fprintf scan_hook_fprintf
#define fputs   scan_hook_fputs
#define fputc   scan_hook_fputc
#include "scanner.c"
#undef main
#undef printf
#undef fprintf
#undef fputs
#undef fputc

/* mesmas mensagens de ERROR_MESSAGES em scanner.py */
static const char *lex_error_message(const char *code) {
    if (!strcmp(code, "UNKNOWN_SYMBOL")) return "simbolo nao reconhecido pela linguagem";
    if (!strcmp(code, "UNTERMINATED_BLOCK_COMMENT")) return "comentario de bloco '/* ... */' nao foi fechado";
    if (!strcmp(code, "UNTERMINATED_CHAR_LITERAL")) return "literal de caractere nao foi fechado com '";
    if (!strcmp(code, "UNTERMINATED_STRING_LITERAL")) return "literal de cadeia nao foi fechado com \"";
    if (!strcmp(code, "MALFORMED_REAL_LITERAL")) return "numero real sem digitos apos o ponto";
    if (!strcmp(code, "INVALID_IDENTIFIER")) return "identificador nao pode comecar com digito";
    if (!strcmp(code, "INCOMPLETE_LOGICAL_OPERATOR")) return "operador logico incompleto (use && ou ||)";
    return code;
}

/* -- leitura das linhas JSON produzidas pelo scanner -------------------- */

/* le uma cadeia JSON a partir de *pp (apontando para a aspa de abertura) */
static char *json_read_string(const char **pp) {
    const char *p = *pp + 1;
    StrBuf b = {0};
    sb_add(&b, "%s", "");
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': sb_add(&b, "\n"); break;
                case 't': sb_add(&b, "\t"); break;
                case 'r': sb_add(&b, "\r"); break;
                case 'b': sb_add(&b, "\b"); break;
                case 'f': sb_add(&b, "\f"); break;
                case 'u': {
                    unsigned v = 0;
                    for (int i = 1; i <= 4 && isxdigit((unsigned char)p[i]); i++)
                        v = v * 16 + (unsigned)(isdigit((unsigned char)p[i])
                                ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10));
                    if (v) sb_add(&b, "%c", (char)v); /* o scanner so usa \u para bytes < 0x20 */
                    p += 4;
                    break;
                }
                default: sb_add(&b, "%c", *p); break; /* \" \\ \/ */
            }
            p++;
        } else {
            sb_add(&b, "%c", *p);
            p++;
        }
    }
    if (*p == '"') p++;
    *pp = p;
    return b.s;
}

/* procura "chave": no objeto JSON da linha e devolve o ponteiro do valor */
static const char *json_find(const char *line, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = line;
    while ((p = strstr(p, pat)) != NULL) {
        /* garante que e uma chave (precedida por '{' ou ','), nao um valor */
        if (p == line || p[-1] == '{' || p[-1] == ',') return p + strlen(pat);
        p++;
    }
    return NULL;
}

static int json_get_int(const char *line, const char *key) {
    const char *p = json_find(line, key);
    return p ? atoi(p) : 0;
}

/* Os campos "token" e "error" vem antes do "lexeme" em cada linha, e o
 * lexema e lido em sequencia, para que um lexema contendo o texto de uma
 * chave (ex.: "\"line\":") nunca seja confundido com um campo. */
static void read_scanner_line(const char *line) {
    const char *p;
    int is_token = strncmp(line, "{\"token\":", 9) == 0;
    int is_error = strncmp(line, "{\"error\":", 9) == 0;
    if (!is_token && !is_error) return;
    p = line + 9;
    char *kind = json_read_string(&p);           /* tipo do token / codigo */
    if (strncmp(p, ",\"lexeme\":", 10) != 0) return;
    p += 10;
    char *lexeme = json_read_string(&p);
    /* o restante (attribute, line, column) nao contem o lexema */
    int line_no = json_get_int(p, "line");
    int col_no = json_get_int(p, "column");
    if (is_token) {
        Token *t = xmalloc(sizeof(Token));
        t->type = kind;
        t->lexeme = lexeme;
        t->line = line_no;
        t->column = col_no;
        list_push(&g_tokens, t);
    } else {
        LexError *e = xmalloc(sizeof(LexError));
        e->error = kind;
        e->lexeme = lexeme;
        e->line = line_no;
        e->column = col_no;
        list_push(&g_lexerrs, e);
    }
}

static void read_scanner_output(const StrBuf *b) {
    if (!b->s) return;
    const char *p = b->s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xstrndup(p, n);
        read_scanner_line(line);
        p += n + (nl ? 1 : 0);
    }
}

/* O scanner.c registra so o primeiro byte de um simbolo desconhecido
 * multibyte (ex.: 'ç'). Para a mensagem ficar igual a do parser em Python,
 * o lexema e completado com os bytes de continuacao UTF-8 do codigo-fonte. */
static void complete_utf8_lexeme(LexError *e) {
    if (strlen(e->lexeme) != 1 || ((unsigned char)e->lexeme[0] & 0xC0) != 0xC0) return;
    size_t i = 0;
    int line = 1, col = 1;
    while (i < g_len && !(line == e->line && col == e->column)) {
        unsigned char c = (unsigned char)g_src[i++];
        if (c == '\n') { line++; col = 1; }
        else if ((c & 0xC0) != 0x80) col++;
    }
    /* avanca ate o inicio do code point na coluna encontrada */
    while (i < g_len && ((unsigned char)g_src[i] & 0xC0) == 0x80) i++;
    if (i >= g_len) return;
    size_t j = i + 1;
    while (j < g_len && ((unsigned char)g_src[j] & 0xC0) == 0x80) j++;
    e->lexeme = xstrndup(g_src + i, j - i);
}

/* roda o scanner da Etapa 1 sobre g_src e le a saida dele */
static void run_scanner(void) {
    scan_all();
    read_scanner_output(&g_scan_stdout);
    read_scanner_output(&g_scan_stderr);
    for (int i = 0; i < g_lexerrs.n; i++) complete_utf8_lexeme((LexError *)g_lexerrs.items[i]);
    if (g_tokens.n == 0) { /* nunca acontece: o scanner sempre emite EOF */
        Token *t = xmalloc(sizeof(Token));
        t->type = "EOF";
        t->lexeme = "";
        t->line = g_line;
        t->column = g_col;
        list_push(&g_tokens, t);
    }
}

/* ===================================================================== */
/* AST                                                                    */
/* ===================================================================== */

typedef struct Node {
    const char *kind;        /* Program, Function, VarDecl, Param, Binary, ... */
    int line, col;           /* posicao do token que originou o no */
    const char *type;        /* VarDecl/Param/Function: tipo; Lit: int/real/... */
    const char *name;        /* VarDecl/Param/Function/Id */
    const char *op;          /* Binary/Unary */
    const char *value;       /* Lit: lexema original */
    int is_array;            /* VarDecl/Param com [ ] */
    PtrList kids;            /* subarvores (NULL = ramo ausente) */
    PtrList params;          /* Function: Param* */
} Node;

static Node *new_node(const char *kind, const Token *at) {
    Node *n = xmalloc(sizeof(Node));
    n->kind = kind;
    n->line = at ? at->line : 1;
    n->col = at ? at->column : 1;
    return n;
}

static void add_kid(Node *n, Node *kid) { list_push(&n->kids, kid); }
#define KID(n, i) ((Node *)(n)->kids.items[i])

/* impressao canonica compacta (identica a _s() em parser.py) */
static void print_compact(StrBuf *b, const Node *n) {
    if (!n) { sb_add(b, "NULL"); return; }
    const char *k = n->kind;
    if (!strcmp(k, "Program") || !strcmp(k, "Block")) {
        sb_add(b, "%s(", k);
        for (int i = 0; i < n->kids.n; i++) {
            if (i) sb_add(b, ", ");
            print_compact(b, KID(n, i));
        }
        sb_add(b, ")");
    } else if (!strcmp(k, "VarDecl") || !strcmp(k, "Param")) {
        int is_var = !strcmp(k, "VarDecl");
        if (is_var) sb_add(b, "VarDecl(");
        sb_add(b, "%s %s", n->type, n->name);
        if (n->is_array) {
            sb_add(b, " size=");
            print_compact(b, KID(n, 0));
        } else if (is_var && n->kids.n && KID(n, 0)) {
            sb_add(b, "=");
            print_compact(b, KID(n, 0));
        }
        if (is_var) sb_add(b, ")");
    } else if (!strcmp(k, "Function")) {
        sb_add(b, "Function(%s %s(", n->type, n->name);
        for (int i = 0; i < n->params.n; i++) {
            if (i) sb_add(b, ",");
            print_compact(b, (Node *)n->params.items[i]);
        }
        sb_add(b, ") ");
        print_compact(b, KID(n, 0));
        sb_add(b, ")");
    } else if (!strcmp(k, "Id")) {
        sb_add(b, "Id(%s)", n->name);
    } else if (!strcmp(k, "Lit")) {
        sb_add(b, "Lit(%s,%s)", n->type, n->value);
    } else if (!strcmp(k, "Binary") || !strcmp(k, "Unary")) {
        sb_add(b, "%s(%s", k, n->op);
        for (int i = 0; i < n->kids.n; i++) {
            sb_add(b, ",");
            print_compact(b, KID(n, i));
        }
        sb_add(b, ")");
    } else if (!strcmp(k, "Break") || !strcmp(k, "Continue")) {
        sb_add(b, "%s", k);
    } else { /* If, While, For, Return, ExprStmt, Assign, Call, Index */
        sb_add(b, "%s(", k);
        for (int i = 0; i < n->kids.n; i++) {
            if (i) sb_add(b, ",");
            print_compact(b, KID(n, i));
        }
        sb_add(b, ")");
    }
}

/* impressao indentada (identica a _tree_lines() em parser.py) */
static void print_tree(StrBuf *b, const Node *n, int depth) {
    char pad[512];
    int p = depth * 2 < (int)sizeof(pad) - 1 ? depth * 2 : (int)sizeof(pad) - 1;
    memset(pad, ' ', (size_t)p);
    pad[p] = '\0';
    if (!n) { sb_add(b, "%sNULL\n", pad); return; }
    const char *k = n->kind;
    if (!strcmp(k, "Lit")) { sb_add(b, "%sLit type=%s value=%s\n", pad, n->type, n->value); return; }
    if (!strcmp(k, "Id")) { sb_add(b, "%sId name=%s\n", pad, n->name); return; }
    if (!strcmp(k, "VarDecl") || !strcmp(k, "Param")) {
        sb_add(b, "%s%s type=%s name=%s%s\n", pad, k, n->type, n->name, n->is_array ? " array" : "");
        for (int i = 0; i < n->kids.n; i++)
            if (KID(n, i) || n->is_array) print_tree(b, KID(n, i), depth + 1);
        return;
    }
    if (!strcmp(k, "Function")) {
        sb_add(b, "%sFunction type=%s name=%s\n", pad, n->type, n->name);
        sb_add(b, "%s  Params%s\n", pad, n->params.n ? "" : " (vazio)");
        for (int i = 0; i < n->params.n; i++) print_tree(b, (Node *)n->params.items[i], depth + 2);
        print_tree(b, KID(n, 0), depth + 1);
        return;
    }
    if (!strcmp(k, "Binary") || !strcmp(k, "Unary")) sb_add(b, "%s%s op=%s\n", pad, k, n->op);
    else if (!strcmp(k, "Block") && n->kids.n == 0) sb_add(b, "%sBlock (vazio)\n", pad);
    else sb_add(b, "%s%s\n", pad, k);
    for (int i = 0; i < n->kids.n; i++) print_tree(b, KID(n, i), depth + 1);
}

/* ===================================================================== */
/* PARSER (descida recursiva)                                             */
/* ===================================================================== */

#define TYPE_NAMES "tipo (int, float, bool, char ou void)"
#define EXPR_START "identificador, literal ou '('"

typedef struct {
    int line, col;
    char *msg;
} SyntaxError;

static PtrList g_synerrs;          /* SyntaxError* */
static int g_tp = 0;               /* indice do token atual */
static int g_ok_since_error = 3;   /* tokens consumidos desde o ultimo erro */
static jmp_buf *g_recover = NULL;  /* ponto de recuperacao atual */

static Token *tok_at(int i) {
    if (i >= g_tokens.n) i = g_tokens.n - 1;
    return (Token *)g_tokens.items[i];
}
static Token *cur(void) { return tok_at(g_tp); }
static const char *cur_type(void) { return cur()->type; }
static int at(const char *t) { return !strcmp(cur_type(), t); }
static int at_type_kw(void) {
    return at("INT") || at("FLOAT") || at("BOOL") || at("CHAR") || at("VOID");
}

static Token *next_tok(void) {
    Token *t = cur();
    if (strcmp(t->type, "EOF") != 0) g_tp++;
    g_ok_since_error++;
    return t;
}

static const char *symbol_of(const char *type) {
    if (!strcmp(type, "LPAREN")) return "'('";
    if (!strcmp(type, "RPAREN")) return "')'";
    if (!strcmp(type, "LBRACKET")) return "'['";
    if (!strcmp(type, "RBRACKET")) return "']'";
    if (!strcmp(type, "LBRACE")) return "'{'";
    if (!strcmp(type, "RBRACE")) return "'}'";
    if (!strcmp(type, "COMMA")) return "','";
    if (!strcmp(type, "SEMICOLON")) return "';'";
    if (!strcmp(type, "ASSIGN")) return "'='";
    if (!strcmp(type, "IDENT")) return "identificador";
    return type;
}

static void describe(StrBuf *b, const Token *t) {
    if (!strcmp(t->type, "EOF")) sb_add(b, "fim do arquivo");
    else if (!strcmp(t->type, "IDENT")) sb_add(b, "identificador '%s'", t->lexeme);
    else if (!strcmp(t->type, "INT_LIT") || !strcmp(t->type, "FLOAT_LIT") ||
             !strcmp(t->type, "CHAR_LIT") || !strcmp(t->type, "STRING_LIT"))
        sb_add(b, "literal %s", t->lexeme);
    else sb_add(b, "'%s'", t->lexeme);
}

/* registra o erro (se nao for cascata) e desvia para a recuperacao */
static void report(const Token *t, char *msg) {
    if (g_ok_since_error >= 3) {
        SyntaxError *e = xmalloc(sizeof(SyntaxError));
        e->line = t->line;
        e->col = t->column;
        e->msg = msg;
        list_push(&g_synerrs, e);
    }
    g_ok_since_error = 0;
    longjmp(*g_recover, 1);
}

/* erro no formato "esperado X, mas foi encontrado Y" */
static void error_expected(const char *fmt, ...) {
    char expected[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(expected, sizeof(expected), fmt, ap);
    va_end(ap);
    StrBuf b = {0};
    sb_add(&b, "esperado %s, mas foi encontrado ", expected);
    describe(&b, cur());
    report(cur(), b.s);
}

static void unexpected(const char *what) {
    StrBuf b = {0};
    sb_add(&b, "token ");
    describe(&b, cur());
    sb_add(&b, " inesperado: %s", what);
    report(cur(), b.s);
}

static Token *expect(const char *type, const char *context) {
    if (at(type)) return next_tok();
    error_expected("%s %s", symbol_of(type), context);
    return NULL; /* inalcancavel */
}

/* -- recuperacao (modo panico) --------------------------------------- */

static void sync_statement(void) {
    int depth = 0;
    while (!at("EOF")) {
        if (at("LBRACE")) depth++;
        else if (at("RBRACE")) {
            if (depth == 0) return;
            depth--;
            if (depth == 0) { g_tp++; return; }
        } else if (at("SEMICOLON") && depth == 0) { g_tp++; return; }
        g_tp++;
    }
}

static void sync_global(void) {
    int depth = 0;
    while (!at("EOF")) {
        int lb = at("LBRACE"), rb = at("RBRACE"), sc = at("SEMICOLON");
        g_tp++;
        if (lb) depth++;
        else if (rb) {
            if (depth <= 1) return;
            depth--;
        } else if (sc && depth == 0) return;
    }
}

/* -- prototipos ------------------------------------------------------- */

static Node *expression(void);
static Node *statement(void);
static Node *block(void);

/* -- expressoes ------------------------------------------------------- */

static Node *literal(void) {
    Token *t = next_tok();
    Node *n = new_node("Lit", t);
    if (!strcmp(t->type, "INT_LIT")) n->type = "int";
    else if (!strcmp(t->type, "FLOAT_LIT")) n->type = "real";
    else if (!strcmp(t->type, "CHAR_LIT")) n->type = "char";
    else if (!strcmp(t->type, "STRING_LIT")) n->type = "string";
    else n->type = "bool";
    n->value = t->lexeme;
    return n;
}

static Node *primary(void) {
    if (at("INT_LIT") || at("FLOAT_LIT") || at("CHAR_LIT") || at("STRING_LIT") ||
        at("TRUE") || at("FALSE"))
        return literal();
    if (at("LPAREN")) {
        next_tok();
        Node *inner = expression(); /* parenteses nao geram no na AST */
        expect("RPAREN", "para fechar a expressao entre parenteses");
        return inner;
    }
    error_expected("expressao (%s)", EXPR_START);
    return NULL;
}

static Node *call(Node *callee) {
    next_tok(); /* '(' */
    Node *n = new_node("Call", NULL);
    n->line = callee->line;
    n->col = callee->col;
    add_kid(n, callee);
    if (!at("RPAREN")) {
        add_kid(n, expression());
        while (at("COMMA")) {
            next_tok();
            if (at("RPAREN")) error_expected("expressao (argumento) apos ','");
            add_kid(n, expression());
        }
    }
    char ctx[300];
    snprintf(ctx, sizeof(ctx), "para fechar a chamada de '%s'", callee->name);
    expect("RPAREN", ctx);
    return n;
}

static Node *postfix(void) {
    Token *t = cur();
    if (at("IDENT") || at("PRINT") || at("READ")) {
        next_tok();
        Node *id = new_node("Id", t);
        id->name = t->lexeme;
        if (at("LPAREN")) return call(id);
        if (strcmp(t->type, "IDENT") != 0) error_expected("'(' apos '%s'", t->lexeme);
        Node *node = id;
        while (at("LBRACKET")) {
            Token *br = next_tok();
            if (at("RBRACKET")) error_expected("expressao de indice (%s)", EXPR_START);
            Node *index = expression();
            char ctx[300];
            snprintf(ctx, sizeof(ctx), "para fechar o indice de '%s'", t->lexeme);
            expect("RBRACKET", ctx);
            Node *ix = new_node("Index", br);
            add_kid(ix, node);
            add_kid(ix, index);
            node = ix;
        }
        return node;
    }
    return primary();
}

static Node *unary(void) {
    if (at("MINUS") || at("NOT") || at("PLUS")) {
        Token *op = next_tok();
        Node *n = new_node("Unary", op);
        n->op = op->lexeme;
        add_kid(n, unary());
        return n;
    }
    return postfix();
}

/* niveis de precedencia, do mais fraco para o mais forte */
static const char *const LEVELS[][4] = {
    {"OR", NULL},
    {"AND", NULL},
    {"EQ", "NEQ", NULL},
    {"LT", "LE", "GT", "GE"},
    {"PLUS", "MINUS", NULL},
    {"STAR", "SLASH", "PERCENT", NULL},
};
#define N_LEVELS ((int)(sizeof(LEVELS) / sizeof(LEVELS[0])))

static int at_level(int level) {
    for (int i = 0; i < 4 && LEVELS[level][i]; i++)
        if (at(LEVELS[level][i])) return 1;
    return 0;
}

static Node *binary(int level) {
    if (level == N_LEVELS) return unary();
    Node *left = binary(level + 1);
    while (at_level(level)) {
        Token *op = next_tok();
        Node *right = binary(level + 1);
        Node *n = new_node("Binary", op);
        n->op = op->lexeme;
        add_kid(n, left);
        add_kid(n, right);
        left = n;
    }
    return left;
}

/* atribuicao -> alvo '=' atribuicao | ou_logico  (associativa a direita).
 * O '=' so continua a atribuicao se o lado esquerdo for Id ou Index. */
static Node *assignment(void) {
    Node *left = binary(0);
    if (at("ASSIGN") && (!strcmp(left->kind, "Id") || !strcmp(left->kind, "Index"))) {
        Token *t = next_tok();
        Node *value = assignment();
        Node *n = new_node("Assign", t);
        add_kid(n, left);
        add_kid(n, value);
        return n;
    }
    return left;
}

static Node *expression(void) { return assignment(); }

/* -- declaracoes ------------------------------------------------------ */

static Token *type_spec(const char *context) {
    if (at_type_kw()) return next_tok();
    error_expected("%s %s", TYPE_NAMES, context);
    return NULL;
}

static Node *var_decl_rest(Token *ttok, Token *name) {
    Node *n = new_node("VarDecl", ttok);
    n->type = ttok->lexeme;
    n->name = name->lexeme;
    if (at("LBRACKET")) {
        next_tok();
        if (!at("INT_LIT")) error_expected("tamanho do vetor '%s' (literal inteiro)", name->lexeme);
        add_kid(n, literal());
        n->is_array = 1;
        char ctx[300];
        snprintf(ctx, sizeof(ctx), "para fechar o tamanho do vetor '%s'", name->lexeme);
        expect("RBRACKET", ctx);
    } else if (at("ASSIGN")) {
        next_tok();
        add_kid(n, expression());
    }
    if (!at("SEMICOLON")) error_expected("';' apos a declaracao de '%s'", name->lexeme);
    next_tok();
    return n;
}

static Node *param(void) {
    Token *ttok = type_spec("do parametro");
    char ctx[300];
    snprintf(ctx, sizeof(ctx), "(nome do parametro) apos o tipo '%s'", ttok->lexeme);
    Token *name = expect("IDENT", ctx);
    Node *n = new_node("Param", ttok);
    n->type = ttok->lexeme;
    n->name = name->lexeme;
    if (at("LBRACKET")) {
        next_tok();
        Node *size = NULL;
        if (at("INT_LIT")) size = literal();
        n->is_array = 1;
        add_kid(n, size);
        snprintf(ctx, sizeof(ctx), "para fechar o vetor '%s'", name->lexeme);
        expect("RBRACKET", ctx);
    }
    return n;
}

static Node *function_rest(Token *ttok, Token *name) {
    next_tok(); /* '(' */
    Node *f = new_node("Function", ttok);
    f->type = ttok->lexeme;
    f->name = name->lexeme;
    if (at("VOID") && !strcmp(tok_at(g_tp + 1)->type, "RPAREN")) {
        next_tok(); /* (void) == lista vazia */
    } else if (!at("RPAREN")) {
        if (!at_type_kw())
            error_expected("%s do parametro ou ')' na lista de parametros de '%s'",
                           TYPE_NAMES, name->lexeme);
        list_push(&f->params, param());
        while (at("COMMA")) {
            next_tok();
            list_push(&f->params, param());
        }
    }
    if (!at("RPAREN"))
        error_expected("',' ou ')' na lista de parametros de '%s'", name->lexeme);
    next_tok(); /* ')' */
    if (!at("LBRACE"))
        error_expected("'{' para iniciar o corpo da funcao '%s'", name->lexeme);
    add_kid(f, block());
    return f;
}

static Node *global_declaration(void) {
    Token *ttok = type_spec("");
    char ctx[300];
    snprintf(ctx, sizeof(ctx), "(nome da variavel ou funcao) apos o tipo '%s'", ttok->lexeme);
    Token *name = expect("IDENT", ctx);
    if (at("LPAREN")) return function_rest(ttok, name);
    return var_decl_rest(ttok, name);
}

static Node *local_declaration(void) {
    Token *ttok = next_tok();
    char ctx[300];
    snprintf(ctx, sizeof(ctx), "(nome da variavel) apos o tipo '%s'", ttok->lexeme);
    Token *name = expect("IDENT", ctx);
    if (at("LPAREN")) unexpected("funcoes nao podem ser declaradas dentro de um bloco");
    return var_decl_rest(ttok, name);
}

/* -- comandos --------------------------------------------------------- */

static Node *block(void) {
    Token *open = next_tok(); /* '{' */
    Node *b = new_node("Block", open);
    while (!at("RBRACE") && !at("EOF")) {
        jmp_buf here;
        jmp_buf *saved = g_recover;
        g_recover = &here;
        if (setjmp(here) == 0) {
            Node *item = at_type_kw() ? local_declaration() : statement();
            add_kid(b, item);
        } else {
            sync_statement();
        }
        g_recover = saved;
    }
    if (at("EOF"))
        error_expected("'}' para fechar o bloco aberto na linha %d, coluna %d",
                       open->line, open->column);
    next_tok(); /* '}' */
    return b;
}

static Node *condition(const char *keyword) {
    char ctx[64];
    snprintf(ctx, sizeof(ctx), "apos '%s'", keyword);
    expect("LPAREN", ctx);
    Node *c = expression();
    snprintf(ctx, sizeof(ctx), "para fechar a condicao do '%s'", keyword);
    expect("RPAREN", ctx);
    return c;
}

static Node *if_statement(void) {
    Token *t = next_tok();
    Node *n = new_node("If", t);
    add_kid(n, condition("if"));
    add_kid(n, statement());
    Node *other = NULL;
    if (at("ELSE")) { /* else pendente: associa ao if mais proximo */
        next_tok();
        other = statement();
    }
    add_kid(n, other);
    return n;
}

static Node *while_statement(void) {
    Token *t = next_tok();
    Node *n = new_node("While", t);
    add_kid(n, condition("while"));
    add_kid(n, statement());
    return n;
}

static Node *for_statement(void) {
    Token *t = next_tok();
    Node *n = new_node("For", t);
    expect("LPAREN", "apos 'for'");
    add_kid(n, at("SEMICOLON") ? NULL : expression());
    expect("SEMICOLON", "apos a inicializacao do 'for'");
    add_kid(n, at("SEMICOLON") ? NULL : expression());
    expect("SEMICOLON", "apos a condicao do 'for'");
    add_kid(n, at("RPAREN") ? NULL : expression());
    expect("RPAREN", "para fechar o cabecalho do 'for'");
    add_kid(n, statement());
    return n;
}

static Node *return_statement(void) {
    Token *t = next_tok();
    Node *n = new_node("Return", t);
    Node *value = NULL;
    if (!at("SEMICOLON")) value = expression();
    if (!at("SEMICOLON")) error_expected("';' apos o 'return'");
    next_tok();
    add_kid(n, value);
    return n;
}

static Node *statement(void) {
    Token *t = cur();
    if (at("LBRACE")) return block();
    if (at("IF")) return if_statement();
    if (at("WHILE")) return while_statement();
    if (at("FOR")) return for_statement();
    if (at("RETURN")) return return_statement();
    if (at("BREAK") || at("CONTINUE")) {
        next_tok();
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "apos '%s'", t->lexeme);
        expect("SEMICOLON", ctx);
        return new_node(!strcmp(t->type, "BREAK") ? "Break" : "Continue", t);
    }
    if (at("ELSE")) unexpected("'else' sem um 'if' correspondente");
    if (at("RBRACE") || at("EOF")) error_expected("inicio de comando (statement)");
    if (at_type_kw()) unexpected("declaracoes so sao permitidas diretamente em um bloco");
    Node *expr = expression();
    if (!at("SEMICOLON")) error_expected("';' apos a expressao");
    next_tok();
    Node *n = new_node("ExprStmt", t);
    add_kid(n, expr);
    return n;
}

static Node *parse_program(void) {
    Node *prog = new_node("Program", NULL);
    while (!at("EOF")) {
        jmp_buf here;
        jmp_buf *saved = g_recover;
        g_recover = &here;
        if (setjmp(here) == 0) {
            Node *item;
            if (at_type_kw()) item = global_declaration();
            else if (at("RBRACE")) { unexpected("nao ha bloco aberto para ser fechado"); item = NULL; }
            else item = statement();
            add_kid(prog, item);
        } else {
            sync_global();
        }
        g_recover = saved;
    }
    return prog;
}

/* ===================================================================== */
/* Diagnosticos e main                                                    */
/* ===================================================================== */

typedef struct {
    int line, col, order;
    char *text;
} Diagnostic;

static void print_location(int line, int col) {
    /* localiza a linha 'line' do codigo-fonte */
    const char *p = g_src, *end = g_src + g_len;
    int cur_line = 1;
    while (cur_line < line && p < end) {
        if (*p == '\n') cur_line++;
        p++;
    }
    fprintf(stderr, "  %4d | ", line);
    if (cur_line == line) {
        for (; p < end && *p != '\n'; p++) fputc(*p == '\t' ? ' ' : *p, stderr);
    }
    fprintf(stderr, "\n       | ");
    for (int i = 1; i < col; i++) fputc(' ', stderr);
    fprintf(stderr, "^\n");
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int show_tokens = 0, as_tree = 0, nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tokens")) show_tokens = 1;
        else if (!strcmp(argv[i], "--arvore")) as_tree = 1;
        else if (!strncmp(argv[i], "--", 2)) continue;
        else { path = argv[i]; nfiles++; }
    }
    if (nfiles != 1) {
        fprintf(stderr, "uso: ./parser codigo.c [--arvore] [--tokens]\n");
        return 2;
    }
    g_src = read_file(path, &g_len);
    if (!g_src) {
        fprintf(stderr, "erro ao abrir '%s'\n", path);
        return 2;
    }

    run_scanner();

    if (show_tokens) {
        printf("TOKENS\n");
        for (int i = 0; i < g_tokens.n; i++) {
            Token *t = (Token *)g_tokens.items[i];
            printf("  %4d:%-4d %-12s %s\n", t->line, t->column, t->type,
                   t->lexeme[0] ? t->lexeme : "<eof>");
        }
        printf("\n");
    }

    Node *program = parse_program();

    /* junta erros lexicos e sintaticos, ordenados por posicao (estavel) */
    int nd = g_lexerrs.n + g_synerrs.n;
    Diagnostic *diags = xmalloc(sizeof(Diagnostic) * (size_t)(nd ? nd : 1));
    int k = 0;
    for (int i = 0; i < g_lexerrs.n; i++) {
        LexError *e = (LexError *)g_lexerrs.items[i];
        StrBuf b = {0};
        sb_add(&b, "Erro lexico na linha %d, coluna %d: %s ('%s')", e->line, e->column,
               lex_error_message(e->error), e->lexeme);
        diags[k++] = (Diagnostic){e->line, e->column, 0, b.s};
    }
    for (int i = 0; i < g_synerrs.n; i++) {
        SyntaxError *e = (SyntaxError *)g_synerrs.items[i];
        StrBuf b = {0};
        sb_add(&b, "Erro de sintaxe na linha %d, coluna %d: %s", e->line, e->col, e->msg);
        diags[k++] = (Diagnostic){e->line, e->col, 1, b.s};
    }
    for (int i = 1; i < nd; i++) { /* insertion sort: estavel */
        Diagnostic d = diags[i];
        int j = i - 1;
        while (j >= 0 && (diags[j].line > d.line ||
                          (diags[j].line == d.line && diags[j].col > d.col) ||
                          (diags[j].line == d.line && diags[j].col == d.col &&
                           diags[j].order > d.order))) {
            diags[j + 1] = diags[j];
            j--;
        }
        diags[j + 1] = d;
    }

    int status = 0;
    if (nd > 0) {
        fflush(stdout);
        for (int i = 0; i < nd; i++) {
            fprintf(stderr, "%s\n", diags[i].text);
            print_location(diags[i].line, diags[i].col);
        }
        fprintf(stderr, "Entrada rejeitada: %d erro(s) encontrado(s); nenhuma AST foi gerada.\n", nd);
        status = 1;
    } else {
        StrBuf b = {0};
        if (as_tree) print_tree(&b, program, 0);
        else { print_compact(&b, program); sb_add(&b, "\n"); }
        fputs(b.s, stdout);
    }

    free(g_src);
    free_all();
    return status;
}
