/*
 * scanner.c - Analisador lexico (scanner) da linguagem MINIC, em C.
 *
 * Implementa exatamente a mesma especificacao lexica e as mesmas regras
 * de recuperacao de erro do scanner em Python (src/scanner.py) e do
 * documento docs/especificacao-lexica.md, para que os dois produzam a
 * mesma sequencia de tokens para qualquer entrada.
 *
 * Uso:
 *   ./scanner arquivo.minic
 *   ./scanner arquivo.c
 *
 * Saida: um objeto JSON por linha em stdout (tokens), no mesmo formato de
 * *.expected.jsonl. Diagnosticos de erro, se houver, vao um por linha em
 * stderr, no mesmo formato de *.errors.jsonl. O codigo de saida e 0
 * quando o arquivo foi lido e analisado com sucesso (mesmo que existam
 * erros lexicos no codigo analisado); so retorna diferente de 0 se o
 * arquivo nao puder ser aberto ou os argumentos forem invalidos.
 *
 * Compilacao:
 *   gcc -Wall -Wextra -std=c11 scanner.c -o scanner
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------- */
/* Estado global do scanner (arquivo pequeno, escopo didatico)          */
/* ------------------------------------------------------------------- */

static char *g_src = NULL;
static size_t g_len = 0;
static size_t g_pos = 0;
static int g_line = 1;
static int g_col = 1;

/* ------------------------------------------------------------------- */
/* Tabelas de tokens                                                    */
/* ------------------------------------------------------------------- */

typedef struct {
    const char *word;
    const char *type;
} Reserved;

static const Reserved RESERVED[] = {
    {"int", "INT"},       {"float", "FLOAT"},   {"bool", "BOOL"},
    {"char", "CHAR"},     {"void", "VOID"},     {"if", "IF"},
    {"else", "ELSE"},     {"while", "WHILE"},   {"for", "FOR"},
    {"return", "RETURN"}, {"break", "BREAK"},   {"continue", "CONTINUE"},
    {"true", "TRUE"},     {"false", "FALSE"},   {"print", "PRINT"},
    {"read", "READ"},
};
#define N_RESERVED (sizeof(RESERVED) / sizeof(RESERVED[0]))

typedef struct {
    char ch;
    const char *type;
} SingleCharToken;

static const SingleCharToken SINGLE_CHAR_TOKENS[] = {
    {'(', "LPAREN"}, {')', "RPAREN"}, {'[', "LBRACKET"}, {']', "RBRACKET"},
    {'{', "LBRACE"}, {'}', "RBRACE"}, {',', "COMMA"},    {';', "SEMICOLON"},
    {'.', "DOT"},    {'+', "PLUS"},   {'-', "MINUS"},    {'*', "STAR"},
    {'%', "PERCENT"},
};
#define N_SINGLE (sizeof(SINGLE_CHAR_TOKENS) / sizeof(SINGLE_CHAR_TOKENS[0]))

/* caracteres que iniciam algum simbolo/operador/delimitador reconhecido;
 * usados apenas na recuperacao de erro de cadeia nao terminada. */
static int is_punct_start(int c) {
    static const char *PUNCT = "()[]{},;.+-*/%<>=!&|";
    return c != 0 && strchr(PUNCT, c) != NULL;
}

/* ------------------------------------------------------------------- */
/* Leitura do codigo-fonte (com suporte simples a UTF-8)                */
/* ------------------------------------------------------------------- */

static int peek(int offset) {
    size_t i = g_pos + (size_t)offset;
    if (offset < 0 || i >= g_len) return -1;
    return (unsigned char)g_src[i];
}

/* consome 1 byte cru, atualizando linha/coluna. Bytes de continuacao
 * UTF-8 (10xxxxxx) nao avancam a coluna, pois fazem parte do caractere
 * anterior. */
static int advance_byte(void) {
    unsigned char c = (unsigned char)g_src[g_pos++];
    if (c == '\n') {
        g_line++;
        g_col = 1;
    } else if ((c & 0xC0) != 0x80) {
        g_col++;
    }
    return c;
}

/* consome 1 caractere logico (1 a 4 bytes se for UTF-8 multibyte) e
 * retorna o byte inicial (suficiente para comparacoes ASCII). */
static int advance(void) {
    int c = advance_byte();
    if ((unsigned char)c >= 0x80) {
        int extra = 0;
        if (((unsigned char)c & 0xE0) == 0xC0) extra = 1;
        else if (((unsigned char)c & 0xF0) == 0xE0) extra = 2;
        else if (((unsigned char)c & 0xF8) == 0xF0) extra = 3;
        for (int i = 0; i < extra && g_pos < g_len; i++) advance_byte();
    }
    return c;
}

static int at_end(void) { return g_pos >= g_len; }

/* conta quantos caracteres logicos (nao bytes de continuacao) existem
 * em src[from, to). Usado para recalcular coluna apos "devolver"
 * caracteres na recuperacao de cadeia nao terminada. */
static size_t codepoint_count(const char *from, const char *to) {
    size_t n = 0;
    for (const char *p = from; p < to; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80) n++;
    }
    return n;
}

static int c_is_digit(int c) { return c >= '0' && c <= '9'; }
static int c_is_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int c_is_alnum(int c) { return c_is_digit(c) || c_is_alpha(c); }

/* ------------------------------------------------------------------- */
/* Saida JSON                                                            */
/* ------------------------------------------------------------------- */

static void json_escape_write(FILE *out, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\t': fputs("\\t", out); break;
            case '\r': fputs("\\r", out); break;
            default:
                if (c < 0x20) {
                    fprintf(out, "\\u%04x", c);
                } else {
                    fputc(c, out);
                }
        }
    }
}

static long long parse_int(const char *s, size_t len) {
    long long v = 0;
    for (size_t i = 0; i < len; i++) v = v * 10 + (s[i] - '0');
    return v;
}

static double parse_double(const char *s, size_t len) {
    char buf[64];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return strtod(buf, NULL);
}

/* formata um double como numero JSON em notacao decimal (nunca
 * cientifica, pois os reais da MINIC nunca usam expoente), removendo
 * zeros a direita desnecessarios mas mantendo pelo menos 1 casa. */
static void format_double_json(double v, char *out, size_t outsz) {
    snprintf(out, outsz, "%.10f", v);
    char *dot = strchr(out, '.');
    if (dot) {
        char *end = out + strlen(out) - 1;
        while (end > dot + 1 && *end == '0') {
            *end = '\0';
            end--;
        }
    }
}

static void emit_null(const char *type, const char *lex, size_t lex_len, int line, int col) {
    printf("{\"token\":\"%s\",\"lexeme\":\"", type);
    json_escape_write(stdout, lex, lex_len);
    printf("\",\"attribute\":null,\"line\":%d,\"column\":%d}\n", line, col);
}

static void emit_int_token(const char *type, const char *lex, size_t lex_len,
                            long long value, int line, int col) {
    printf("{\"token\":\"%s\",\"lexeme\":\"", type);
    json_escape_write(stdout, lex, lex_len);
    printf("\",\"attribute\":%lld,\"line\":%d,\"column\":%d}\n", value, line, col);
}

static void emit_float_token(const char *type, const char *lex, size_t lex_len,
                              double value, int line, int col) {
    char numbuf[64];
    format_double_json(value, numbuf, sizeof(numbuf));
    printf("{\"token\":\"%s\",\"lexeme\":\"", type);
    json_escape_write(stdout, lex, lex_len);
    printf("\",\"attribute\":%s,\"line\":%d,\"column\":%d}\n", numbuf, line, col);
}

static void emit_string_attr(const char *type, const char *lex, size_t lex_len,
                              const char *attr, size_t attr_len, int line, int col) {
    printf("{\"token\":\"%s\",\"lexeme\":\"", type);
    json_escape_write(stdout, lex, lex_len);
    printf("\",\"attribute\":\"");
    json_escape_write(stdout, attr, attr_len);
    printf("\",\"line\":%d,\"column\":%d}\n", line, col);
}

static void emit_error(const char *err, const char *lex, size_t lex_len, int line, int col) {
    fprintf(stderr, "{\"error\":\"%s\",\"lexeme\":\"", err);
    json_escape_write(stderr, lex, lex_len);
    fprintf(stderr, "\",\"line\":%d,\"column\":%d}\n", line, col);
}

/* ------------------------------------------------------------------- */
/* Reconhecedores                                                        */
/* ------------------------------------------------------------------- */

static const char *lookup_reserved(const char *lex, size_t len) {
    for (size_t i = 0; i < N_RESERVED; i++) {
        if (strlen(RESERVED[i].word) == len && strncmp(RESERVED[i].word, lex, len) == 0) {
            return RESERVED[i].type;
        }
    }
    return NULL;
}

static void scan_line_comment(void) {
    advance(); /* '/' */
    advance(); /* '/' */
    while (!at_end() && peek(0) != '\n') advance();
}

static void scan_block_comment(void) {
    int start_line = g_line, start_col = g_col;
    size_t start_pos = g_pos;
    advance(); /* '/' */
    advance(); /* '*' */
    int closed = 0;
    while (!at_end()) {
        if (peek(0) == '*' && peek(1) == '/') {
            advance();
            advance();
            closed = 1;
            break;
        }
        advance();
    }
    if (!closed) {
        emit_error("UNTERMINATED_BLOCK_COMMENT", g_src + start_pos, g_pos - start_pos,
                    start_line, start_col);
    }
}

static void scan_identifier(void) {
    int start_line = g_line, start_col = g_col;
    size_t start_pos = g_pos;
    while (c_is_alnum(peek(0))) advance();
    size_t len = g_pos - start_pos;
    const char *lex = g_src + start_pos;
    const char *type = lookup_reserved(lex, len);
    if (type) {
        emit_null(type, lex, len, start_line, start_col);
    } else {
        emit_string_attr("IDENT", lex, len, lex, len, start_line, start_col);
    }
}

static void scan_number(void) {
    int start_line = g_line, start_col = g_col;
    size_t start_pos = g_pos;
    while (c_is_digit(peek(0))) advance();

    /* identificador invalido: digitos seguidos imediatamente de letra/_ */
    if (c_is_alpha(peek(0))) {
        while (c_is_alnum(peek(0))) advance();
        const char *full = g_src + start_pos;
        size_t full_len = g_pos - start_pos;
        emit_error("INVALID_IDENTIFIER", full, full_len, start_line, start_col);

        size_t digits_len = 0;
        while (digits_len < full_len && c_is_digit(full[digits_len])) digits_len++;
        long long intval = parse_int(full, digits_len);
        emit_int_token("INT_LIT", full, digits_len, intval, start_line, start_col);

        const char *ident_part = full + digits_len;
        size_t ident_len = full_len - digits_len;
        int ident_col = start_col + (int)digits_len;
        const char *type = lookup_reserved(ident_part, ident_len);
        if (type) {
            emit_null(type, ident_part, ident_len, start_line, ident_col);
        } else {
            emit_string_attr("IDENT", ident_part, ident_len, ident_part, ident_len,
                              start_line, ident_col);
        }
        return;
    }

    /* real bem formado: [0-9]+ '.' [0-9]+ */
    if (peek(0) == '.' && c_is_digit(peek(1))) {
        advance(); /* '.' */
        while (c_is_digit(peek(0))) advance();
        const char *lex = g_src + start_pos;
        size_t len = g_pos - start_pos;
        double val = parse_double(lex, len);
        emit_float_token("FLOAT_LIT", lex, len, val, start_line, start_col);
        return;
    }

    /* real malformado: '.' sem digito depois -> backoff para INT_LIT */
    if (peek(0) == '.' && peek(1) != '.') {
        const char *malformed = g_src + start_pos;
        size_t malformed_len = (g_pos - start_pos) + 1; /* inclui o '.' */
        emit_error("MALFORMED_REAL_LITERAL", malformed, malformed_len, start_line, start_col);

        const char *int_lex = g_src + start_pos;
        size_t int_len = g_pos - start_pos;
        long long intval = parse_int(int_lex, int_len);
        emit_int_token("INT_LIT", int_lex, int_len, intval, start_line, start_col);
        /* o '.' nao e consumido: sera relido como DOT na proxima chamada */
        return;
    }

    const char *lex = g_src + start_pos;
    size_t len = g_pos - start_pos;
    long long intval = parse_int(lex, len);
    emit_int_token("INT_LIT", lex, len, intval, start_line, start_col);
}

static const char ESCAPE_CHARS[] = "ntr\\'\"0";
static const char ESCAPE_VALUES[] = "\n\t\r\\'\"\0";

static size_t decode_escapes(const char *raw, size_t raw_len, char *out) {
    size_t oi = 0, i = 0;
    while (i < raw_len) {
        if (raw[i] == '\\' && i + 1 < raw_len) {
            char e = raw[i + 1];
            const char *p = strchr(ESCAPE_CHARS, e);
            out[oi++] = p ? ESCAPE_VALUES[p - ESCAPE_CHARS] : e;
            i += 2;
        } else {
            out[oi++] = raw[i++];
        }
    }
    return oi;
}

static void scan_char_literal(void) {
    int start_line = g_line, start_col = g_col;
    size_t start_pos = g_pos;
    advance(); /* abre ' */

    size_t content_start = g_pos;
    size_t content_len = 0;
    if (!at_end() && peek(0) != '\'' && peek(0) != '\n') {
        int c = advance();
        content_len = g_pos - content_start;
        if (c == '\\' && !at_end() && peek(0) != '\n') {
            advance();
            content_len = g_pos - content_start;
        }
    }

    if (!at_end() && peek(0) == '\'') {
        advance(); /* fecha ' */
        const char *lex = g_src + start_pos;
        size_t lex_len = g_pos - start_pos;
        char decoded[16];
        size_t dlen = decode_escapes(g_src + content_start, content_len, decoded);
        emit_string_attr("CHAR_LIT", lex, lex_len, decoded, dlen, start_line, start_col);
        return;
    }

    /* nao fechou: erro. lexema reportado = "'" + conteudo lido */
    char err_lex[20];
    err_lex[0] = '\'';
    size_t n = content_len < sizeof(err_lex) - 1 ? content_len : sizeof(err_lex) - 1;
    memcpy(err_lex + 1, g_src + content_start, n);
    emit_error("UNTERMINATED_CHAR_LITERAL", err_lex, n + 1, start_line, start_col);

    /* recuperacao: descarta tambem o caractere que deveria ter sido o
     * fechamento, se existir e nao for quebra de linha/EOF. */
    if (!at_end() && peek(0) != '\n') advance();
}

static void scan_string_literal(void) {
    int start_line = g_line, start_col = g_col;
    size_t start_pos = g_pos;
    advance(); /* abre " */

    char content[4096];
    size_t content_len = 0;
    int closed = 0;

    while (!at_end() && peek(0) != '\n') {
        if (peek(0) == '"') {
            advance();
            closed = 1;
            break;
        }
        size_t before = g_pos;
        int c = advance();
        if (c == '\\' && !at_end() && peek(0) != '\n') {
            if (content_len + 2 < sizeof(content)) {
                content[content_len++] = (char)c;
            }
            size_t before2 = g_pos;
            int c2 = advance();
            (void)before2;
            if (content_len + 1 < sizeof(content)) content[content_len++] = (char)c2;
        } else {
            size_t nbytes = g_pos - before;
            if (content_len + nbytes < sizeof(content)) {
                memcpy(content + content_len, g_src + before, nbytes);
                content_len += nbytes;
            }
        }
    }

    if (closed) {
        const char *lex = g_src + start_pos;
        size_t lex_len = g_pos - start_pos;
        char decoded[4096];
        size_t dlen = decode_escapes(content, content_len, decoded);
        emit_string_attr("STRING_LIT", lex, lex_len, decoded, dlen, start_line, start_col);
        return;
    }

    /* nao fechou ate a quebra de linha (ou EOF): erro. */
    size_t eol_pos = g_pos;
    const char *diag_lex = g_src + start_pos;
    size_t diag_len = eol_pos - start_pos;
    emit_error("UNTERMINATED_STRING_LITERAL", diag_lex, diag_len, start_line, start_col);

    /* recuperacao: volta ate o primeiro caractere de pontuacao/operador
     * reconhecido (se houver), para que o restante da linha continue
     * sendo tokenizado normalmente. */
    size_t content_start = start_pos + 1;
    size_t recovery_pos = eol_pos;
    for (size_t i = content_start; i < eol_pos; i++) {
        if (is_punct_start((unsigned char)g_src[i])) {
            recovery_pos = i;
            break;
        }
    }
    size_t delta_chars = codepoint_count(g_src + content_start, g_src + recovery_pos);
    g_pos = recovery_pos;
    g_col = start_col + 1 + (int)delta_chars;
}

static void scan_operator(void) {
    int line = g_line, col = g_col;
    int ch = advance();

    if (ch == '=') {
        if (peek(0) == '=') { advance(); emit_null("EQ", "==", 2, line, col); }
        else emit_null("ASSIGN", "=", 1, line, col);
        return;
    }
    if (ch == '!') {
        if (peek(0) == '=') { advance(); emit_null("NEQ", "!=", 2, line, col); }
        else emit_null("NOT", "!", 1, line, col);
        return;
    }
    if (ch == '<') {
        if (peek(0) == '=') { advance(); emit_null("LE", "<=", 2, line, col); }
        else emit_null("LT", "<", 1, line, col);
        return;
    }
    if (ch == '>') {
        if (peek(0) == '=') { advance(); emit_null("GE", ">=", 2, line, col); }
        else emit_null("GT", ">", 1, line, col);
        return;
    }
    if (ch == '&') {
        if (peek(0) == '&') { advance(); emit_null("AND", "&&", 2, line, col); }
        else emit_error("INCOMPLETE_LOGICAL_OPERATOR", "&", 1, line, col);
        return;
    }
    if (ch == '|') {
        if (peek(0) == '|') { advance(); emit_null("OR", "||", 2, line, col); }
        else emit_error("INCOMPLETE_LOGICAL_OPERATOR", "|", 1, line, col);
        return;
    }
    if (ch == '/') {
        emit_null("SLASH", "/", 1, line, col);
        return;
    }
}

static void scan_token(void) {
    int ch = peek(0);

    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        advance();
        return;
    }

    if (ch == '/' && peek(1) == '/') { scan_line_comment(); return; }
    if (ch == '/' && peek(1) == '*') { scan_block_comment(); return; }

    if (c_is_digit(ch)) { scan_number(); return; }
    if (c_is_alpha(ch)) { scan_identifier(); return; }

    if (ch == '\'') { scan_char_literal(); return; }
    if (ch == '"') { scan_string_literal(); return; }

    if (ch == '=' || ch == '!' || ch == '<' || ch == '>' || ch == '&' ||
        ch == '|' || ch == '/') {
        scan_operator();
        return;
    }

    for (size_t i = 0; i < N_SINGLE; i++) {
        if (SINGLE_CHAR_TOKENS[i].ch == ch) {
            int line = g_line, col = g_col;
            char lex[2] = {(char)ch, 0};
            advance();
            emit_null(SINGLE_CHAR_TOKENS[i].type, lex, 1, line, col);
            return;
        }
    }

    /* simbolo desconhecido */
    int line = g_line, col = g_col;
    char lex[2] = {(char)ch, 0};
    advance();
    emit_error("UNKNOWN_SYMBOL", lex, 1, line, col);
}

static void scan_all(void) {
    while (!at_end()) scan_token();
    emit_null("EOF", "", 0, g_line, g_col);
}

/* ------------------------------------------------------------------- */
/* main                                                                   */
/* ------------------------------------------------------------------- */

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    *out_len = read;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: %s arquivo.minic\n", argv[0]);
        return 2;
    }

    g_src = read_file(argv[1], &g_len);
    if (!g_src) {
        fprintf(stderr, "erro ao abrir '%s'\n", argv[1]);
        return 2;
    }

    scan_all();

    free(g_src);
    return 0;
}
