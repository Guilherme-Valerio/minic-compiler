#!/usr/bin/env python3
r"""
scanner.py - Analisador lexico (scanner) da linguagem MINIC.

Uso:
    python scanner.py arquivo.minic
    python scanner.py arquivo.minic --format text
    python scanner.py arquivo.minic --out tokens.jsonl

Por padrao, a saida e JSON Lines: um token por linha em stdout (mesmo
formato de *.expected.jsonl) e os diagnosticos, se houver, um por linha em
stderr (mesmo formato de *.errors.jsonl). Use --format text para uma
tabela legivel no terminal.

O scanner tambem pode ser usado como biblioteca:

    from scanner import tokenize
    tokens, errors = tokenize(codigo_fonte)

Convencoes adotadas (ver README.md e docs/especificacao-lexica.md):
  - Palavras reservadas: int, float, bool, char, void, if, else, while,
    for, return, break, continue, true, false, print, read.
  - Identificadores: [A-Za-z_][A-Za-z0-9_]*
  - Inteiros: [0-9]+                -> INT_LIT (atributo numerico)
  - Reais:    [0-9]+\.[0-9]+        -> FLOAT_LIT (atributo numerico)
  - Operadores compostos tem prioridade sobre os simples (maximal munch):
    ==, !=, <=, >=, &&, ||
  - Delimitadores: ( ) [ ] { } , ;
  - Comentarios // e /* ... */ sao ignorados (nao geram token).
  - Literais opcionais: CHAR_LIT ('c') e STRING_LIT ("texto").
  - A coluna comeca em 1, a linha comeca em 1.
  - EOF e sempre o ultimo token da sequencia.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, asdict
from typing import Any, List, Optional, Tuple


# ---------------------------------------------------------------------------
# Tabelas de tokens
# ---------------------------------------------------------------------------

RESERVED = {
    "int": "INT",
    "float": "FLOAT",
    "bool": "BOOL",
    "char": "CHAR",
    "void": "VOID",
    "if": "IF",
    "else": "ELSE",
    "while": "WHILE",
    "for": "FOR",
    "return": "RETURN",
    "break": "BREAK",
    "continue": "CONTINUE",
    "true": "TRUE",
    "false": "FALSE",
    "print": "PRINT",
    "read": "READ",
}

# Delimitadores/operadores de um unico caractere que nao exigem lookahead.
SINGLE_CHAR_TOKENS = {
    "(": "LPAREN",
    ")": "RPAREN",
    "[": "LBRACKET",
    "]": "RBRACKET",
    "{": "LBRACE",
    "}": "RBRACE",
    ",": "COMMA",
    ";": "SEMICOLON",
    ".": "DOT",
    "+": "PLUS",
    "-": "MINUS",
    "*": "STAR",
    "%": "PERCENT",
}

# Caracteres que iniciam algum simbolo/operador/delimitador reconhecido pela
# linguagem. Usado apenas na recuperacao de erro de cadeia nao terminada,
# para localizar um ponto seguro de resincronizacao (ver README).
PUNCT_START = set("()[]{},;.+-*/%<>=!&|")

ESCAPES = {
    "n": "\n",
    "t": "\t",
    "\\": "\\",
    "'": "'",
    '"': '"',
    "0": "\0",
}


# ---------------------------------------------------------------------------
# Estruturas de dados
# ---------------------------------------------------------------------------

@dataclass
class Token:
    token: str
    lexeme: str
    attribute: Any
    line: int
    column: int

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class LexError:
    error: str
    lexeme: str
    line: int
    column: int

    def to_dict(self) -> dict:
        return asdict(self)


# Mensagens legiveis para o modo texto (nao fazem parte do JSON de saida,
# que segue estritamente o formato combinado com o professor).
ERROR_MESSAGES = {
    "UNKNOWN_SYMBOL": "simbolo nao reconhecido pela linguagem",
    "UNTERMINATED_BLOCK_COMMENT": "comentario de bloco '/* ... */' nao foi fechado",
    "UNTERMINATED_CHAR_LITERAL": "literal de caractere nao foi fechado com '",
    "UNTERMINATED_STRING_LITERAL": 'literal de cadeia nao foi fechado com "',
    "MALFORMED_REAL_LITERAL": "numero real sem digitos apos o ponto",
    "INVALID_IDENTIFIER": "identificador nao pode comecar com digito",
    "INCOMPLETE_LOGICAL_OPERATOR": "operador logico incompleto (use && ou ||)",
}


class Scanner:
    """Percorre o codigo-fonte caractere a caractere produzindo tokens.

    A estrategia de recuperacao de erro e especifica por tipo de erro e
    esta descrita em detalhe no README.md do projeto. Em resumo:

      - simbolo desconhecido: descarta 1 caractere e continua.
      - comentario de bloco sem fechamento: consome ate o EOF.
      - caractere/cadeia mal formados: usam recuperacao propria (ver abaixo).
      - real malformado / identificador invalido: fazem "backoff", isto e,
        o scanner devolve os tokens validos que podem ser extraidos do
        lexema problematico (ex.: "12." -> INT_LIT "12" + DOT ".").
    """

    def __init__(self, source: str):
        self.src = source
        self.pos = 0
        self.line = 1
        self.col = 1
        self.n = len(source)
        self.tokens: List[Token] = []
        self.errors: List[LexError] = []

    # -- utilidades basicas -------------------------------------------------

    def _peek(self, offset: int = 0) -> str:
        i = self.pos + offset
        return self.src[i] if i < self.n else ""

    def _advance(self) -> str:
        ch = self.src[self.pos]
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def _at_end(self) -> bool:
        return self.pos >= self.n

    def _emit(self, token: str, lexeme: str, attribute: Any, line: int, col: int) -> None:
        self.tokens.append(Token(token, lexeme, attribute, line, col))

    def _emit_error(self, error: str, lexeme: str, line: int, col: int) -> None:
        self.errors.append(LexError(error, lexeme, line, col))

    # -- laco principal -------------------------------------------------

    def scan(self) -> Tuple[List[Token], List[LexError]]:
        while not self._at_end():
            self._scan_token()
        self._emit("EOF", "", None, self.line, self.col)
        return self.tokens, self.errors

    def _scan_token(self) -> None:
        ch = self._peek()

        # espacos em branco (nao geram token, apenas movem linha/coluna)
        if ch in " \t\r\n":
            self._advance()
            return

        # comentarios
        if ch == "/" and self._peek(1) == "/":
            self._scan_line_comment()
            return
        if ch == "/" and self._peek(1) == "*":
            self._scan_block_comment()
            return

        # identificadores / palavras reservadas / numeros
        if ch.isdigit():
            self._scan_number()
            return
        if ch.isalpha() or ch == "_":
            self._scan_identifier()
            return

        # literais
        if ch == "'":
            self._scan_char_literal()
            return
        if ch == '"':
            self._scan_string_literal()
            return

        # operadores de dois caracteres (maximal munch) e um caractere
        if ch in "=!<>&|/":
            self._scan_operator()
            return

        if ch in SINGLE_CHAR_TOKENS:
            line, col = self.line, self.col
            self._advance()
            self._emit(SINGLE_CHAR_TOKENS[ch], ch, None, line, col)
            return

        # nada reconheceu o caractere: simbolo desconhecido
        line, col = self.line, self.col
        self._advance()
        self._emit_error("UNKNOWN_SYMBOL", ch, line, col)

    # -- comentarios -------------------------------------------------------

    def _scan_line_comment(self) -> None:
        self._advance()  # '/'
        self._advance()  # '/'
        while not self._at_end() and self._peek() != "\n":
            self._advance()
        # a quebra de linha (se existir) e consumida no proximo _scan_token

    def _scan_block_comment(self) -> None:
        start_line, start_col = self.line, self.col
        start_pos = self.pos
        self._advance()  # '/'
        self._advance()  # '*'
        closed = False
        while not self._at_end():
            if self._peek() == "*" and self._peek(1) == "/":
                self._advance()
                self._advance()
                closed = True
                break
            self._advance()
        if not closed:
            lexeme = self.src[start_pos:self.pos]
            self._emit_error("UNTERMINATED_BLOCK_COMMENT", lexeme, start_line, start_col)

    # -- numeros -------------------------------------------------------

    def _scan_number(self) -> None:
        start_line, start_col = self.line, self.col
        start_pos = self.pos
        while self._peek().isdigit():
            self._advance()

        # identificador invalido: digitos seguidos imediatamente de letra/underscore
        if self._peek().isalpha() or self._peek() == "_":
            while self._peek().isalnum() or self._peek() == "_":
                self._advance()
            full_lexeme = self.src[start_pos:self.pos]
            self._emit_error("INVALID_IDENTIFIER", full_lexeme, start_line, start_col)
            # backoff: divide em INT_LIT (prefixo numerico) + IDENT (resto)
            digits_len = 0
            while digits_len < len(full_lexeme) and full_lexeme[digits_len].isdigit():
                digits_len += 1
            int_part = full_lexeme[:digits_len]
            ident_part = full_lexeme[digits_len:]
            self._emit("INT_LIT", int_part, int(int_part), start_line, start_col)
            ident_col = start_col + digits_len
            token_type = RESERVED.get(ident_part, "IDENT")
            attribute = None if token_type != "IDENT" else ident_part
            self._emit(token_type, ident_part, attribute, start_line, ident_col)
            return

        # real bem formado: [0-9]+ '.' [0-9]+
        if self._peek() == "." and self._peek(1).isdigit():
            self._advance()  # '.'
            while self._peek().isdigit():
                self._advance()
            lexeme = self.src[start_pos:self.pos]
            self._emit("FLOAT_LIT", lexeme, float(lexeme), start_line, start_col)
            return

        # real malformado: '.' sem digito depois -> backoff para INT_LIT
        if self._peek() == "." and self._peek(1) != ".":
            malformed_lexeme = self.src[start_pos:self.pos] + "."
            self._emit_error("MALFORMED_REAL_LITERAL", malformed_lexeme, start_line, start_col)
            int_lexeme = self.src[start_pos:self.pos]
            self._emit("INT_LIT", int_lexeme, int(int_lexeme), start_line, start_col)
            # o '.' nao e consumido: sera relido como DOT na proxima iteracao
            return

        lexeme = self.src[start_pos:self.pos]
        self._emit("INT_LIT", lexeme, int(lexeme), start_line, start_col)

    # -- identificadores / palavras reservadas ------------------------------

    def _scan_identifier(self) -> None:
        start_line, start_col = self.line, self.col
        start_pos = self.pos
        while self._peek().isalnum() or self._peek() == "_":
            self._advance()
        lexeme = self.src[start_pos:self.pos]
        if lexeme in RESERVED:
            self._emit(RESERVED[lexeme], lexeme, None, start_line, start_col)
        else:
            self._emit("IDENT", lexeme, lexeme, start_line, start_col)

    # -- literal de caractere -------------------------------------------------

    def _scan_char_literal(self) -> None:
        start_line, start_col = self.line, self.col
        start_pos = self.pos
        self._advance()  # abre '

        content_raw = ""
        if not self._at_end() and self._peek() != "'" and self._peek() != "\n":
            c = self._advance()
            content_raw += c
            if c == "\\" and not self._at_end() and self._peek() != "\n":
                content_raw += self._advance()

        if not self._at_end() and self._peek() == "'":
            self._advance()  # fecha '
            lexeme = self.src[start_pos:self.pos]
            attribute = self._decode_escapes(content_raw)
            self._emit("CHAR_LIT", lexeme, attribute, start_line, start_col)
            return

        # nao fechou: erro. O lexema reportado e "'" + conteudo lido.
        error_lexeme = "'" + content_raw
        self._emit_error("UNTERMINATED_CHAR_LITERAL", error_lexeme, start_line, start_col)
        # recuperacao: descarta tambem o caractere que deveria ter sido o
        # fechamento (se existir e nao for quebra de linha/EOF), reduzindo a
        # chance de gerar uma cascata de novos erros na mesma linha.
        if not self._at_end() and self._peek() != "\n":
            self._advance()

    # -- literal de cadeia -------------------------------------------------

    def _scan_string_literal(self) -> None:
        start_line, start_col = self.line, self.col
        start_pos = self.pos
        self._advance()  # abre "

        content_raw = ""
        closed = False
        while not self._at_end() and self._peek() != "\n":
            if self._peek() == '"':
                self._advance()
                closed = True
                break
            c = self._advance()
            if c == "\\" and not self._at_end() and self._peek() != "\n":
                content_raw += c + self._advance()
            else:
                content_raw += c

        if closed:
            lexeme = self.src[start_pos:self.pos]
            attribute = self._decode_escapes(content_raw)
            self._emit("STRING_LIT", lexeme, attribute, start_line, start_col)
            return

        # nao fechou ate a quebra de linha (ou EOF): erro.
        # o diagnostico mostra o conteudo ate o fim da linha para dar
        # contexto completo ao usuario.
        eol_pos = self.pos
        diag_lexeme = self.src[start_pos:eol_pos]
        self._emit_error("UNTERMINATED_STRING_LITERAL", diag_lexeme, start_line, start_col)

        # recuperacao: volta ate o primeiro caractere de pontuacao/operador
        # reconhecido pela linguagem (se houver) para que o restante da
        # linha continue sendo tokenizado normalmente. Caso nao exista tal
        # caractere, descarta a linha inteira.
        content_start = start_pos + 1
        recovery_pos = eol_pos
        for i in range(content_start, eol_pos):
            if self.src[i] in PUNCT_START:
                recovery_pos = i
                break
        # "devolve" os caracteres entre a posicao atual e o ponto de
        # recuperacao, recalculando linha/coluna a partir do inicio do
        # literal (nenhuma quebra de linha ocorre nesse trecho).
        self.pos = recovery_pos
        self.col = start_col + 1 + (recovery_pos - content_start)

    @staticmethod
    def _decode_escapes(raw: str) -> str:
        out = []
        i = 0
        while i < len(raw):
            if raw[i] == "\\" and i + 1 < len(raw):
                out.append(ESCAPES.get(raw[i + 1], raw[i + 1]))
                i += 2
            else:
                out.append(raw[i])
                i += 1
        return "".join(out)

    # -- operadores -------------------------------------------------------

    def _scan_operator(self) -> None:
        line, col = self.line, self.col
        ch = self._advance()

        two_char = {
            "=": ("=", "EQ"),
            "!": ("=", "NEQ"),
            "<": ("=", "LE"),
            ">": ("=", "GE"),
            "&": ("&", "AND"),
            "|": ("|", "OR"),
        }

        if ch in two_char:
            expected_next, token_name = two_char[ch]
            if self._peek() == expected_next:
                self._advance()
                self._emit(token_name, ch + expected_next, None, line, col)
                return
            if ch == "=":
                self._emit("ASSIGN", "=", None, line, col)
                return
            if ch == "!":
                self._emit("NOT", "!", None, line, col)
                return
            if ch == "<":
                self._emit("LT", "<", None, line, col)
                return
            if ch == ">":
                self._emit("GT", ">", None, line, col)
                return
            if ch in ("&", "|"):
                # operador logico isolado: nao faz parte da linguagem
                self._emit_error("INCOMPLETE_LOGICAL_OPERATOR", ch, line, col)
                return

        if ch == "/":
            self._emit("SLASH", "/", None, line, col)
            return


def tokenize(source: str) -> Tuple[List[Token], List[LexError]]:
    """API principal para uso do scanner como biblioteca."""
    return Scanner(source).scan()


# ---------------------------------------------------------------------------
# Saida em modo texto (terminal) e em modo JSON Lines
# ---------------------------------------------------------------------------

def format_text(tokens: List[Token], errors: List[LexError]) -> str:
    lines = []
    lines.append("TOKENS")
    lines.append("-" * 72)
    lines.append(f"{'#':>4}  {'TOKEN':<12}{'LEXEMA':<20}{'ATRIBUTO':<16}{'LINHA':>6}{'COL':>5}")
    for i, t in enumerate(tokens, start=1):
        attr = "" if t.attribute is None else repr(t.attribute)
        lexeme_display = t.lexeme if t.lexeme != "" else "<eof>"
        lines.append(
            f"{i:>4}  {t.token:<12}{lexeme_display:<20}{attr:<16}{t.line:>6}{t.column:>5}"
        )

    lines.append("")
    lines.append("DIAGNOSTICOS")
    lines.append("-" * 72)
    if not errors:
        lines.append("(nenhum erro lexico encontrado)")
    else:
        for e in errors:
            msg = ERROR_MESSAGES.get(e.error, "")
            lines.append(
                f"[{e.line}:{e.column}] {e.error}: '{e.lexeme}' - {msg}"
            )

    lines.append("")
    lines.append(f"Total de tokens: {len(tokens)}  |  Total de erros: {len(errors)}")
    return "\n".join(lines)


def format_jsonl(tokens: List[Token], errors: List[LexError]) -> Tuple[str, str]:
    dumps = lambda d: json.dumps(d, ensure_ascii=False, separators=(",", ":"))
    tokens_jsonl = "\n".join(dumps(t.to_dict()) for t in tokens)
    errors_jsonl = "\n".join(dumps(e.to_dict()) for e in errors)
    return tokens_jsonl, errors_jsonl


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Scanner (analisador lexico) da linguagem MINIC."
    )
    parser.add_argument("arquivo", help="arquivo-fonte a ser analisado (.minic ou .c)")
    parser.add_argument(
        "--format",
        choices=["jsonl", "text"],
        default="jsonl",
        help="formato da saida (padrao: jsonl, um token JSON por linha em "
        "stdout; use 'text' para uma tabela legivel no terminal)",
    )
    parser.add_argument(
        "--out",
        help="se informado, grava os tokens neste arquivo em vez de imprimir "
        "no terminal (no formato jsonl, os erros vao para <out>.errors.jsonl)",
    )
    args = parser.parse_args(argv)

    try:
        with open(args.arquivo, "r", encoding="utf-8") as f:
            source = f.read()
    except OSError as exc:
        print(f"erro ao abrir '{args.arquivo}': {exc}", file=sys.stderr)
        return 2

    tokens, errors = tokenize(source)

    if args.format == "text":
        output = format_text(tokens, errors)
        if args.out:
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(output + "\n")
        else:
            print(output)
    else:
        # Formato padrao: tokens em JSON Lines no stdout (um objeto por
        # linha, no mesmo formato de *.expected.jsonl), diagnosticos em
        # JSON Lines no stderr (um objeto por linha, no mesmo formato de
        # *.errors.jsonl). Isso permite `python scanner.py arquivo.c >
        # saida.jsonl` diretamente, sem flags extras.
        tokens_jsonl, errors_jsonl = format_jsonl(tokens, errors)
        if args.out:
            with open(args.out, "w", encoding="utf-8") as f:
                if tokens_jsonl:
                    f.write(tokens_jsonl + "\n")
            errors_path = args.out + ".errors.jsonl"
            with open(errors_path, "w", encoding="utf-8") as f:
                if errors_jsonl:
                    f.write(errors_jsonl + "\n")
        else:
            if tokens_jsonl:
                print(tokens_jsonl)
            if errors_jsonl:
                print(errors_jsonl, file=sys.stderr)

    # O codigo de saida so indica falha de execucao (ex.: arquivo nao
    # encontrado). Encontrar erros lexicos no codigo analisado nao e uma
    # falha do scanner em si, entao o encerramento continua sendo 0.
    return 0


if __name__ == "__main__":
    sys.exit(main())
