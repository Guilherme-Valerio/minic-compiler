#!/usr/bin/env python3
r"""
parser.py - Analisador sintatico (parser) da linguagem MINIC.

Uso:
    python parser.py codigo.c              # AST compacta (uma linha) no stdout
    python parser.py codigo.c --arvore     # AST indentada, um no por linha
    python parser.py codigo.c --tokens     # mostra tambem os tokens do scanner

Integracao com a Etapa 1: os tokens vem do scanner (scanner.py, no mesmo
diretorio), sem nenhuma alteracao nele. O parser consome a lista de Token
(tipo, lexema, atributo, linha, coluna) e usa EOF como marcador de fim.

Estrategia: descida recursiva (analise descendente preditiva, LL(1)), com
uma funcao por nao-terminal da gramatica documentada em
docs/especificacao-sintatica.md. Cada funcao devolve o no da AST que
reconheceu.

Saida:
  - entrada aceita: AST em S-expressao no stdout, codigo de saida 0;
  - entrada rejeitada: diagnosticos no stderr (tipo do erro, linha, coluna,
    o que era esperado e o que foi encontrado, com a linha do codigo e um
    marcador ^), nenhuma AST, codigo de saida 1;
  - falha de execucao (arquivo inexistente, argumentos): codigo 2.

Recuperacao de erros (modo panico): ao encontrar um erro, o parser registra
o diagnostico, descarta tokens ate um ponto de sincronizacao (';' ou '}' no
mesmo nivel de chaves) e continua, para poder relatar mais de um erro. Para
evitar mensagens em cascata, um novo erro so e relatado depois que o parser
consumir pelo menos 3 tokens normalmente desde o erro anterior (a mesma
heuristica usada pelo yacc).
"""

from __future__ import annotations

import os
import sys
from typing import List, Optional

# o scanner da Etapa 1 fica no mesmo diretorio deste arquivo
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scanner import tokenize, Token, ERROR_MESSAGES  # noqa: E402


# ---------------------------------------------------------------------------
# AST
# ---------------------------------------------------------------------------

class Node:
    """No generico da AST: categoria, atributos escalares, filhos e posicao.

    kind   -> categoria (Program, Function, VarDecl, Binary, ...)
    attrs  -> atributos escalares em ordem fixa (tipo, nome, operador, ...)
    kids   -> subarvores; None representa ramo ausente (impresso como NULL)
    line/col -> posicao do token que originou o no (para a analise semantica)
    """

    def __init__(self, kind: str, line: int, col: int, attrs=None, kids=None):
        self.kind = kind
        self.line = line
        self.col = col
        self.attrs = attrs or {}
        self.kids = kids or []


def _s(node: Optional[Node]) -> str:
    """Impressao canonica compacta (S-expressao em uma linha).

    Convencao (a mesma do parser em C): argumentos de um no separados por
    ',' sem espaco; itens de listas de declaracoes/comandos (Program e
    Block) separados por ', '.
    """
    if node is None:
        return "NULL"
    k, a = node.kind, node.attrs
    if k == "Program":
        return "Program(" + ", ".join(_s(c) for c in node.kids) + ")"
    if k == "Block":
        return "Block(" + ", ".join(_s(c) for c in node.kids) + ")"
    if k == "VarDecl":
        text = a["type"] + " " + a["name"]
        if a.get("is_array"):
            text += " size=" + _s(node.kids[0])
        elif node.kids and node.kids[0] is not None:
            text += "=" + _s(node.kids[0])
        return "VarDecl(" + text + ")"
    if k == "Param":
        text = a["type"] + " " + a["name"]
        if a.get("is_array"):
            text += " size=" + _s(node.kids[0])
        return text
    if k == "Function":
        params = ",".join(_s(p) for p in a["params"])
        return "Function(%s %s(%s) %s)" % (a["type"], a["name"], params, _s(node.kids[0]))
    if k == "Id":
        return "Id(" + a["name"] + ")"
    if k == "Lit":
        return "Lit(%s,%s)" % (a["type"], a["value"])
    if k in ("Binary", "Unary"):
        return k + "(" + ",".join([a["op"]] + [_s(c) for c in node.kids]) + ")"
    if k in ("Break", "Continue"):
        return k
    # If, While, For, Return, ExprStmt, Assign, Call, Index
    return k + "(" + ",".join(_s(c) for c in node.kids) + ")"


def _tree_lines(node: Optional[Node], depth: int, out: List[str]) -> None:
    """Impressao indentada (um no por linha, dois espacos por nivel)."""
    pad = "  " * depth
    if node is None:
        out.append(pad + "NULL")
        return
    k, a = node.kind, node.attrs
    if k == "Lit":
        out.append("%sLit type=%s value=%s" % (pad, a["type"], a["value"]))
        return
    if k == "Id":
        out.append("%sId name=%s" % (pad, a["name"]))
        return
    if k in ("VarDecl", "Param"):
        extra = " array" if a.get("is_array") else ""
        out.append("%s%s type=%s name=%s%s" % (pad, k, a["type"], a["name"], extra))
        for c in node.kids:
            if c is not None or a.get("is_array"):
                _tree_lines(c, depth + 1, out)
        return
    if k == "Function":
        out.append("%sFunction type=%s name=%s" % (pad, a["type"], a["name"]))
        out.append(pad + "  Params" + ("" if a["params"] else " (vazio)"))
        for p in a["params"]:
            _tree_lines(p, depth + 2, out)
        _tree_lines(node.kids[0], depth + 1, out)
        return
    if k in ("Binary", "Unary"):
        out.append("%s%s op=%s" % (pad, k, a["op"]))
    elif k == "Block" and not node.kids:
        out.append(pad + "Block (vazio)")
    else:
        out.append(pad + k)
    for c in node.kids:
        _tree_lines(c, depth + 1, out)


def format_tree(node: Node) -> str:
    out: List[str] = []
    _tree_lines(node, 0, out)
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Descricoes legiveis de tokens (usadas nas mensagens de erro)
# ---------------------------------------------------------------------------

TYPE_TOKENS = {"INT", "FLOAT", "BOOL", "CHAR", "VOID"}
TYPE_NAMES = "tipo (int, float, bool, char ou void)"

SYMBOL_OF = {
    "LPAREN": "'('", "RPAREN": "')'", "LBRACKET": "'['", "RBRACKET": "']'",
    "LBRACE": "'{'", "RBRACE": "'}'", "COMMA": "','", "SEMICOLON": "';'",
    "ASSIGN": "'='", "IDENT": "identificador",
}

EXPR_START = "identificador, literal ou '('"


def describe(tok: Token) -> str:
    if tok.token == "EOF":
        return "fim do arquivo"
    if tok.token == "IDENT":
        return "identificador '%s'" % tok.lexeme
    if tok.token in ("INT_LIT", "FLOAT_LIT", "CHAR_LIT", "STRING_LIT"):
        return "literal %s" % tok.lexeme
    return "'%s'" % tok.lexeme


# ---------------------------------------------------------------------------
# Parser (descida recursiva)
# ---------------------------------------------------------------------------

class ParseError(Exception):
    pass


BINARY_LEVELS = [
    ("OR",),
    ("AND",),
    ("EQ", "NEQ"),
    ("LT", "LE", "GT", "GE"),
    ("PLUS", "MINUS"),
    ("STAR", "SLASH", "PERCENT"),
]

STATEMENT_KEYWORDS = {"IF", "WHILE", "FOR", "RETURN", "BREAK", "CONTINUE", "LBRACE"}


class Parser:
    def __init__(self, tokens: List[Token]):
        self.toks = tokens
        self.pos = 0
        self.errors: List[tuple] = []  # (linha, coluna, mensagem)
        self.ok_since_error = 3         # tokens consumidos desde o ultimo erro

    # -- utilidades -----------------------------------------------------

    @property
    def cur(self) -> Token:
        return self.toks[self.pos]

    def peek(self, k: int = 1) -> Token:
        i = min(self.pos + k, len(self.toks) - 1)
        return self.toks[i]

    def at(self, *types: str) -> bool:
        return self.cur.token in types

    def advance(self) -> Token:
        tok = self.cur
        if tok.token != "EOF":
            self.pos += 1
        self.ok_since_error += 1
        return tok

    def error(self, expected: str, tok: Optional[Token] = None) -> None:
        """Registra um erro 'esperado X, encontrado Y' e interrompe a regra."""
        tok = tok or self.cur
        msg = "esperado %s, mas foi encontrado %s" % (expected, describe(tok))
        self.report(tok, msg)

    def unexpected(self, what: str) -> None:
        tok = self.cur
        self.report(tok, "token %s inesperado: %s" % (describe(tok), what))

    def report(self, tok: Token, msg: str) -> None:
        if self.ok_since_error >= 3:
            self.errors.append((tok.line, tok.column, msg))
        self.ok_since_error = 0
        raise ParseError(msg)

    def expect(self, ttype: str, context: str) -> Token:
        if self.cur.token == ttype:
            return self.advance()
        self.error("%s %s" % (SYMBOL_OF.get(ttype, ttype), context))
        raise AssertionError  # inalcancavel

    # -- recuperacao (modo panico) --------------------------------------

    def sync_statement(self) -> None:
        """Descarta tokens ate ';' (consumido) ou '}' (nao consumido) no
        mesmo nivel de chaves em que o erro ocorreu, ou ate o EOF."""
        depth = 0
        while not self.at("EOF"):
            t = self.cur.token
            if t == "LBRACE":
                depth += 1
            elif t == "RBRACE":
                if depth == 0:
                    return
                depth -= 1
                if depth == 0:
                    self.pos += 1
                    return
            elif t == "SEMICOLON" and depth == 0:
                self.pos += 1
                return
            self.pos += 1

    def sync_global(self) -> None:
        """Descarta o restante da declaracao global com erro: ate um ';' fora
        de chaves ou ate a '}' que fecha o corpo iniciado nela."""
        depth = 0
        while not self.at("EOF"):
            t = self.cur.token
            self.pos += 1
            if t == "LBRACE":
                depth += 1
            elif t == "RBRACE":
                if depth <= 1:
                    return
                depth -= 1
            elif t == "SEMICOLON" and depth == 0:
                return

    # -- programa -------------------------------------------------------

    def parse_program(self) -> Node:
        """program -> ( declaracao_global | comando )* EOF"""
        items = []
        while not self.at("EOF"):
            try:
                if self.at(*TYPE_TOKENS):
                    items.append(self.global_declaration())
                elif self.at("RBRACE"):
                    self.unexpected("nao ha bloco aberto para ser fechado")
                else:
                    items.append(self.statement())
            except ParseError:
                self.sync_global()
        return Node("Program", 1, 1, kids=items)

    def type_spec(self, context: str) -> Token:
        if self.at(*TYPE_TOKENS):
            return self.advance()
        self.error(TYPE_NAMES + " " + context)
        raise AssertionError

    def global_declaration(self) -> Node:
        """declaracao_global -> tipo IDENT ( funcao_resto | variavel_resto )"""
        ttok = self.type_spec("")
        name = self.expect("IDENT", "(nome da variavel ou funcao) apos o tipo '%s'" % ttok.lexeme)
        if self.at("LPAREN"):
            return self.function_rest(ttok, name)
        return self.var_decl_rest(ttok, name)

    def function_rest(self, ttok: Token, name: Token) -> Node:
        """funcao -> tipo IDENT '(' parametros ')' bloco"""
        self.advance()  # '('
        params = []
        if self.at("VOID") and self.peek().token == "RPAREN":
            self.advance()  # (void) == lista vazia
        elif not self.at("RPAREN"):
            if not self.at(*TYPE_TOKENS):
                self.error("%s do parametro ou ')' na lista de parametros de '%s'"
                           % (TYPE_NAMES, name.lexeme))
            params.append(self.param())
            while self.at("COMMA"):
                self.advance()
                params.append(self.param())
        if not self.at("RPAREN"):
            self.error("',' ou ')' na lista de parametros de '%s'" % name.lexeme)
        self.advance()  # ')'
        if not self.at("LBRACE"):
            self.error("'{' para iniciar o corpo da funcao '%s'" % name.lexeme)
        body = self.block()
        return Node("Function", ttok.line, ttok.column,
                    attrs={"type": ttok.lexeme, "name": name.lexeme, "params": params},
                    kids=[body])

    def param(self) -> Node:
        """parametro -> tipo IDENT ( '[' INT_LIT? ']' )?"""
        ttok = self.type_spec("do parametro")
        name = self.expect("IDENT", "(nome do parametro) apos o tipo '%s'" % ttok.lexeme)
        attrs = {"type": ttok.lexeme, "name": name.lexeme}
        kids = []
        if self.at("LBRACKET"):
            self.advance()
            size = None
            if self.at("INT_LIT"):
                size = self.literal()
            attrs["is_array"] = True
            kids = [size]
            self.expect("RBRACKET", "para fechar o vetor '%s'" % name.lexeme)
        return Node("Param", ttok.line, ttok.column, attrs=attrs, kids=kids)

    def var_decl_rest(self, ttok: Token, name: Token) -> Node:
        """variavel -> tipo IDENT ( '[' INT_LIT ']' | '=' expressao )? ';'"""
        attrs = {"type": ttok.lexeme, "name": name.lexeme}
        kids: list = []
        if self.at("LBRACKET"):
            self.advance()
            if not self.at("INT_LIT"):
                self.error("tamanho do vetor '%s' (literal inteiro)" % name.lexeme)
            kids = [self.literal()]
            attrs["is_array"] = True
            self.expect("RBRACKET", "para fechar o tamanho do vetor '%s'" % name.lexeme)
        elif self.at("ASSIGN"):
            self.advance()
            kids = [self.expression()]
        if not self.at("SEMICOLON"):
            self.error("';' apos a declaracao de '%s'" % name.lexeme)
        self.advance()
        return Node("VarDecl", ttok.line, ttok.column, attrs=attrs, kids=kids)

    def local_declaration(self) -> Node:
        ttok = self.advance()
        name = self.expect("IDENT", "(nome da variavel) apos o tipo '%s'" % ttok.lexeme)
        if self.at("LPAREN"):
            self.unexpected("funcoes nao podem ser declaradas dentro de um bloco")
        return self.var_decl_rest(ttok, name)

    # -- comandos -------------------------------------------------------

    def block(self) -> Node:
        """bloco -> '{' ( declaracao_local | comando )* '}'"""
        open_tok = self.advance()  # '{'
        items = []
        while not self.at("RBRACE", "EOF"):
            try:
                if self.at(*TYPE_TOKENS):
                    items.append(self.local_declaration())
                else:
                    items.append(self.statement())
            except ParseError:
                self.sync_statement()
        if self.at("EOF"):
            self.error("'}' para fechar o bloco aberto na linha %d, coluna %d"
                       % (open_tok.line, open_tok.column))
        self.advance()  # '}'
        return Node("Block", open_tok.line, open_tok.column, kids=items)

    def statement(self) -> Node:
        tok = self.cur
        t = tok.token
        if t == "LBRACE":
            return self.block()
        if t == "IF":
            return self.if_statement()
        if t == "WHILE":
            return self.while_statement()
        if t == "FOR":
            return self.for_statement()
        if t == "RETURN":
            return self.return_statement()
        if t in ("BREAK", "CONTINUE"):
            self.advance()
            self.expect("SEMICOLON", "apos '%s'" % tok.lexeme)
            return Node(t.capitalize(), tok.line, tok.column)
        if t == "ELSE":
            self.unexpected("'else' sem um 'if' correspondente")
        if t in ("RBRACE", "EOF"):
            self.error("inicio de comando (statement)")
        if t in TYPE_TOKENS:
            self.unexpected("declaracoes so sao permitidas diretamente em um bloco")
        # comando de expressao
        expr = self.expression()
        if not self.at("SEMICOLON"):
            self.error("';' apos a expressao")
        self.advance()
        return Node("ExprStmt", tok.line, tok.column, kids=[expr])

    def condition(self, keyword: str) -> Node:
        self.expect("LPAREN", "apos '%s'" % keyword)
        cond = self.expression()
        self.expect("RPAREN", "para fechar a condicao do '%s'" % keyword)
        return cond

    def if_statement(self) -> Node:
        """if -> 'if' '(' expr ')' comando ( 'else' comando )?
        O 'else' pendente e associado ao 'if' mais proximo."""
        tok = self.advance()
        cond = self.condition("if")
        then = self.statement()
        other = None
        if self.at("ELSE"):
            self.advance()
            other = self.statement()
        return Node("If", tok.line, tok.column, kids=[cond, then, other])

    def while_statement(self) -> Node:
        tok = self.advance()
        cond = self.condition("while")
        body = self.statement()
        return Node("While", tok.line, tok.column, kids=[cond, body])

    def for_statement(self) -> Node:
        """for -> 'for' '(' expr? ';' expr? ';' expr? ')' comando"""
        tok = self.advance()
        self.expect("LPAREN", "apos 'for'")
        init = None if self.at("SEMICOLON") else self.expression()
        self.expect("SEMICOLON", "apos a inicializacao do 'for'")
        cond = None if self.at("SEMICOLON") else self.expression()
        self.expect("SEMICOLON", "apos a condicao do 'for'")
        step = None if self.at("RPAREN") else self.expression()
        self.expect("RPAREN", "para fechar o cabecalho do 'for'")
        body = self.statement()
        return Node("For", tok.line, tok.column, kids=[init, cond, step, body])

    def return_statement(self) -> Node:
        tok = self.advance()
        value = None
        if not self.at("SEMICOLON"):
            value = self.expression()
        if not self.at("SEMICOLON"):
            self.error("';' apos o 'return'")
        self.advance()
        return Node("Return", tok.line, tok.column, kids=[value])

    # -- expressoes -----------------------------------------------------

    def expression(self) -> Node:
        return self.assignment()

    def assignment(self) -> Node:
        """atribuicao -> alvo '=' atribuicao | ou_logico   (assoc. a direita)

        O '=' so continua a atribuicao se o lado esquerdo for um alvo valido
        (Id ou Index). Caso contrario a expressao termina e quem chamou
        informa o que esperava naquele ponto (ex.: ']' em a[1 = 2])."""
        left = self.binary(0)
        if self.at("ASSIGN") and left.kind in ("Id", "Index"):
            tok = self.advance()
            value = self.assignment()
            return Node("Assign", tok.line, tok.column, kids=[left, value])
        return left

    def binary(self, level: int) -> Node:
        """Um nivel de precedencia por chamada, todos associativos a esquerda."""
        if level == len(BINARY_LEVELS):
            return self.unary()
        left = self.binary(level + 1)
        while self.at(*BINARY_LEVELS[level]):
            op = self.advance()
            right = self.binary(level + 1)
            left = Node("Binary", op.line, op.column, attrs={"op": op.lexeme}, kids=[left, right])
        return left

    def unary(self) -> Node:
        if self.at("MINUS", "NOT", "PLUS"):
            op = self.advance()
            operand = self.unary()
            return Node("Unary", op.line, op.column, attrs={"op": op.lexeme}, kids=[operand])
        return self.postfix()

    def postfix(self) -> Node:
        tok = self.cur
        if self.at("IDENT", "PRINT", "READ"):
            self.advance()
            ident = Node("Id", tok.line, tok.column, attrs={"name": tok.lexeme})
            if self.at("LPAREN"):
                return self.call(ident)
            if tok.token != "IDENT":
                self.error("'(' apos '%s'" % tok.lexeme)
            node = ident
            while self.at("LBRACKET"):
                br = self.advance()
                if self.at("RBRACKET"):
                    self.error("expressao de indice (%s)" % EXPR_START)
                index = self.expression()
                self.expect("RBRACKET", "para fechar o indice de '%s'" % tok.lexeme)
                node = Node("Index", br.line, br.column, kids=[node, index])
            return node
        return self.primary()

    def call(self, callee: Node) -> Node:
        """chamada -> IDENT '(' ( expr ( ',' expr )* )? ')'"""
        self.advance()  # '('
        args = []
        if not self.at("RPAREN"):
            args.append(self.expression())
            while self.at("COMMA"):
                self.advance()
                if self.at("RPAREN"):
                    self.error("expressao (argumento) apos ','")
                args.append(self.expression())
        self.expect("RPAREN", "para fechar a chamada de '%s'" % callee.attrs["name"])
        return Node("Call", callee.line, callee.col, kids=[callee] + args)

    def literal(self) -> Node:
        tok = self.advance()
        kind = {"INT_LIT": "int", "FLOAT_LIT": "real", "CHAR_LIT": "char",
                "STRING_LIT": "string", "TRUE": "bool", "FALSE": "bool"}[tok.token]
        return Node("Lit", tok.line, tok.column, attrs={"type": kind, "value": tok.lexeme})

    def primary(self) -> Node:
        if self.at("INT_LIT", "FLOAT_LIT", "CHAR_LIT", "STRING_LIT", "TRUE", "FALSE"):
            return self.literal()
        if self.at("LPAREN"):
            self.advance()
            inner = self.expression()  # parenteses nao geram no na AST
            self.expect("RPAREN", "para fechar a expressao entre parenteses")
            return inner
        self.error("expressao (%s)" % EXPR_START)
        raise AssertionError


# ---------------------------------------------------------------------------
# Diagnosticos e CLI
# ---------------------------------------------------------------------------

def show_location(lines: List[str], line: int, col: int) -> str:
    if 1 <= line <= len(lines):
        text = lines[line - 1].replace("\t", " ")
    else:
        text = ""
    return "  %4d | %s\n       | %s^" % (line, text, " " * (col - 1))


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    show_tokens = "--tokens" in argv
    as_tree = "--arvore" in argv
    files = [a for a in argv if not a.startswith("--")]
    if len(files) != 1:
        print("uso: python parser.py codigo.c [--arvore] [--tokens]", file=sys.stderr)
        return 2
    path = files[0]
    try:
        with open(path, "r", encoding="utf-8") as f:
            source = f.read()
    except (OSError, UnicodeDecodeError) as exc:
        print("erro ao abrir '%s': %s" % (path, exc), file=sys.stderr)
        return 2

    tokens, lex_errors = tokenize(source)

    if show_tokens:
        print("TOKENS")
        for t in tokens:
            print("  %4d:%-4d %-12s %s" % (t.line, t.column, t.token,
                                          t.lexeme if t.lexeme else "<eof>"))
        print()

    parser = Parser(tokens)
    # a descida recursiva usa ~10 chamadas por nivel de parenteses; o limite
    # padrao do Python (1000) seria atingido com ~100 niveis de aninhamento
    sys.setrecursionlimit(max(sys.getrecursionlimit(), 20000))
    try:
        program = parser.parse_program()
    except RecursionError:
        print("erro: programa aninhado demais para ser analisado", file=sys.stderr)
        return 2

    diagnostics = []  # (linha, coluna, ordem, texto)
    for e in lex_errors:
        text = "Erro lexico na linha %d, coluna %d: %s ('%s')" % (
            e.line, e.column, ERROR_MESSAGES.get(e.error, e.error), e.lexeme)
        diagnostics.append((e.line, e.column, 0, text))
    for (line, col, msg) in parser.errors:
        text = "Erro de sintaxe na linha %d, coluna %d: %s" % (line, col, msg)
        diagnostics.append((line, col, 1, text))

    if diagnostics:
        src_lines = source.split("\n")
        diagnostics.sort(key=lambda d: (d[0], d[1], d[2]))
        for (line, col, _, text) in diagnostics:
            print(text, file=sys.stderr)
            print(show_location(src_lines, line, col), file=sys.stderr)
        print("Entrada rejeitada: %d erro(s) encontrado(s); nenhuma AST foi gerada."
              % len(diagnostics), file=sys.stderr)
        return 1

    print(format_tree(program) if as_tree else _s(program))
    return 0


if __name__ == "__main__":
    sys.exit(main())
