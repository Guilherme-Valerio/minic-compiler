# Especificação sintática da MINIC (Etapa 2)

Este documento descreve o analisador sintático (parser) da MINIC, cobrindo
as três partes da atividade prática da Aula 12: **A — contrato entre scanner
e parser**, **B — AST** e **C — testes**.

O parser foi implementado duas vezes, com exatamente o mesmo comportamento:
`src/parser.py` (Python) e `src/parser.c` (C). Para qualquer entrada, as duas
versões produzem a mesma saída, byte a byte, e o mesmo código de saída (ver
seção 7).

```bash
cd src
python parser.py codigo.c            # Python
gcc -Wall -Wextra -std=c11 parser.c -o parser
./parser codigo.c                    # C

# opções extras (iguais nas duas versões)
python parser.py codigo.c --arvore   # AST indentada, um nó por linha
python parser.py codigo.c --tokens   # lista também os tokens do scanner
```

| Situação | stdout | stderr | código de saída |
|---|---|---|---|
| entrada aceita | AST (uma linha) | vazio | `0` |
| entrada rejeitada | vazio | diagnósticos + resumo | `1` |
| falha de execução (arquivo inexistente, uso incorreto) | vazio | mensagem | `2` |

## 1. Parte A — contrato entre scanner e parser

O parser **não reconhece caracteres**: ele consome a sequência de tokens do
scanner da Etapa 1, que não foi alterado.

- **Python:** `parser.py` importa `tokenize()` diretamente de `scanner.py`
  (mesmo diretório) e recebe a lista de `Token` e a lista de `LexError`.
- **C:** o `scanner.c` da Etapa 1 é um programa completo (tem `main()` e
  imprime os tokens direto no stdout), então não pode ser ligado a outro
  programa sem ser modificado. Como o script do professor compila um único
  arquivo (`gcc parser.c -o parser`), o `parser.c` contém uma cópia dos
  mesmos reconhecedores do `scanner.c`, com uma única diferença: cada token
  é guardado em um vetor em vez de ser impresso. A equivalência com o
  scanner em Python é verificada pelos testes de paridade (seção 7).

Cada token entregue ao parser tem:

| Campo | Uso no parser |
|---|---|
| `token` (tipo) | escolhe a produção (`INT`, `IDENT`, `SEMICOLON`, `LBRACE`, ...) |
| `lexeme` | nome de identificadores, valor de literais, operador de `Binary`/`Unary` e texto dos diagnósticos |
| `line`, `column` | posição dos nós da AST e dos erros (linha e coluna começam em 1) |
| `EOF` | marcador de fim de entrada: sempre o último token; a análise só aceita ao alcançá-lo |

Os nomes dos tipos de token são os da Etapa 1 (`docs/especificacao-lexica.md`).
As pistas dos testes do professor usam outra convenção; a correspondência é:
`PONTO_E_VIRGULA` = `SEMICOLON`, `FECHA_PAREN` = `RPAREN`, `ABRE_CHAVE` =
`LBRACE`, `FECHA_CHAVE` = `RBRACE`, `FECHA_COLCHETE` = `RBRACKET`, `IDENT` =
`IDENT` e `KW_*` = palavras reservadas (`INT`, `ELSE`, ...).

**Erros léxicos.** Se o scanner relatar erros, eles são impressos junto com
os erros sintáticos (ordenados por posição) e a entrada é rejeitada. O parser
continua a análise sobre os tokens que o scanner conseguiu recuperar, para
relatar também os erros sintáticos do mesmo arquivo.

## 2. Gramática

Estratégia: **descida recursiva** (análise descendente preditiva), com uma
função por não-terminal. A Aula 12 destaca que a escolha entre parser
descendente e ascendente não altera o contrato da AST; a descida recursiva
foi escolhida por permitir mensagens de erro específicas de cada construção
("esperado ')' para fechar a condição do 'if'") e por ser implementável de
forma idêntica em Python e em C sem gerador de parser.

Notação: `{ X }` = zero ou mais repetições, `[ X ]` = opcional.

```
programa          → { declaracao_global | comando } EOF
declaracao_global → tipo IDENT ( resto_funcao | resto_variavel )
tipo              → 'int' | 'float' | 'bool' | 'char' | 'void'

resto_funcao      → '(' [ parametros | 'void' ] ')' bloco
parametros        → parametro { ',' parametro }
parametro         → tipo IDENT [ '[' [ INT_LIT ] ']' ]
resto_variavel    → [ '[' INT_LIT ']' | '=' expressao ] ';'

bloco             → '{' { declaracao_local | comando } '}'
declaracao_local  → tipo IDENT resto_variavel

comando           → bloco
                  | 'if' '(' expressao ')' comando [ 'else' comando ]
                  | 'while' '(' expressao ')' comando
                  | 'for' '(' [ expressao ] ';' [ expressao ] ';' [ expressao ] ')' comando
                  | 'return' [ expressao ] ';'
                  | 'break' ';'
                  | 'continue' ';'
                  | expressao ';'

expressao         → atribuicao
atribuicao        → ou [ '=' atribuicao ]          (só se 'ou' for Id ou Index)
ou                → e { '||' e }
e                 → igualdade { '&&' igualdade }
igualdade         → relacional { ( '==' | '!=' ) relacional }
relacional        → aditiva { ( '<' | '<=' | '>' | '>=' ) aditiva }
aditiva           → multiplicativa { ( '+' | '-' ) multiplicativa }
multiplicativa    → unaria { ( '*' | '/' | '%' ) unaria }
unaria            → ( '-' | '+' | '!' ) unaria | posfixa
posfixa           → ( IDENT | 'print' | 'read' ) '(' [ argumentos ] ')'
                  | IDENT { '[' expressao ']' }
                  | primaria
argumentos        → expressao { ',' expressao }
primaria          → INT_LIT | FLOAT_LIT | CHAR_LIT | STRING_LIT
                  | 'true' | 'false' | '(' expressao ')'
```

**Precedência e associatividade** (da menor para a maior):

| Nível | Operadores | Associatividade |
|---|---|---|
| 1 | `=` | direita (`a = b = 3` → `Assign(a, Assign(b, 3))`) |
| 2 | `\|\|` | esquerda |
| 3 | `&&` | esquerda |
| 4 | `==` `!=` | esquerda |
| 5 | `<` `<=` `>` `>=` | esquerda |
| 6 | `+` `-` | esquerda |
| 7 | `*` `/` `%` | esquerda |
| 8 | `-` `+` `!` (unários) | direita |
| 9 | chamada `f(...)`, indexação `v[i]` | — |

**Decisões de projeto:**

- **Recursão à esquerda.** Produções como `E → E + T | T` (Aula 11) não
  podem ser usadas diretamente em descida recursiva. Cada nível foi reescrito
  como `E → T { + T }`, e o laço constrói os nós `Binary` da esquerda para a
  direita, preservando a associatividade à esquerda.
- **Declaração global.** Função e variável começam igual (`tipo IDENT`).
  A gramática foi fatorada: após ler `tipo IDENT`, um `(` decide que é função.
- **Comandos no nível global.** O caso 22 dos testes (`int a; int b; a = b = 3;`)
  exige comandos fora de funções, então o programa aceita declarações e
  comandos misturados.
- **Else pendente.** O `else` é associado ao `if` mais próximo (a regra
  opcional `[ 'else' comando ]` consome o `else` sempre que ele aparece).
- **Alvo da atribuição.** O `=` só continua uma atribuição se o lado esquerdo
  for `Id` ou `Index`. Caso contrário, a expressão termina ali e quem a
  chamou informa o que esperava. Assim `a[1 = 2;` produz "esperado ']'" (o
  erro real) em vez de uma mensagem sobre atribuição.
- **`print` e `read`.** São palavras reservadas no scanner, mas usadas como
  chamadas (`print(total);`). A AST as representa como `Call(Id(print), ...)`,
  sem nó especial: a verificação de que são funções embutidas fica para a
  análise semântica.
- **Vetores.** Na declaração o tamanho é um literal inteiro (`int v[10];`).
  Em parâmetros o tamanho é opcional (`int v[5]` ou `int v[]`).
- **`(void)`** na lista de parâmetros equivale a uma lista vazia.

## 3. Parte B — AST

A AST é construída durante a análise: cada função do parser devolve o nó que
reconheceu, e o nó pai é criado quando todos os filhos já existem (o
equivalente, na descida recursiva, ao momento da redução em um parser LR).
Não terminais auxiliares da gramática (`ou`, `aditiva`, `unaria`, ...) e
delimitadores (`;`, `,`, parênteses) não aparecem na árvore.

Todo nó guarda `line` e `col` do token que o originou, para uso nas
mensagens da análise semântica. Literais guardam o **lexema original**
(`1.0` continua `1.0`, e não `1`).

| Nó | Campos (na ordem de impressão) |
|---|---|
| `Program` | lista de declarações e comandos globais |
| `Function` | tipo de retorno, nome, parâmetros, corpo (`Block`) |
| `VarDecl` | tipo, nome, inicializador **ou** tamanho (`size=`) |
| `Block` | lista ordenada de declarações e comandos |
| `If` | condição, então, senão (`NULL` se ausente) |
| `While` | condição, corpo |
| `For` | inicialização, condição, passo (`NULL` se ausentes), corpo |
| `Return` | expressão (`NULL` em `return;`) |
| `Break`, `Continue` | — |
| `ExprStmt` | expressão usada como comando |
| `Assign` | alvo, valor |
| `Binary` | operador, esquerdo, direito |
| `Unary` | operador, operando |
| `Call` | função (`Id`), argumentos em ordem (lista vazia é válida) |
| `Index` | vetor, índice |
| `Id` | nome |
| `Lit` | tipo (`int`, `real`, `bool`, `char`, `string`), lexema |

### Impressão canônica

A saída padrão é a S-expressão dos testes do professor, em uma linha, com
uma regra fixa de espaçamento:

- argumentos de um nó separados por `,` sem espaço: `Binary(+,Id(a),Lit(int,1))`;
- itens de listas de declarações/comandos (`Program` e `Block`) separados
  por `, `;
- o inicializador de `VarDecl` é impresso como `=valor`; o tamanho de vetor
  como ` size=valor`.

```
$ python parser.py testes/exemplo-aula/exemplo.c
Program(Function(int main() Block(VarDecl(int total=Binary(+,Lit(int,2),Binary(*,Lit(int,3),Lit(int,4)))), ExprStmt(Call(Id(print),Id(total))), Return(Lit(int,0)))))
```

A opção `--arvore` imprime a mesma AST indentada (dois espaços por nível, um
nó por linha), mais confortável para leitura humana:

```
$ python parser.py testes/exemplo-aula/exemplo.c --arvore
Program
  Function type=int name=main
    Params (vazio)
    Block
      VarDecl type=int name=total
        Binary op=+
          Lit type=int value=2
          Binary op=*
            Lit type=int value=3
            Lit type=int value=4
      ExprStmt
        Call
          Id name=print
          Id name=total
      Return
        Lit type=int value=0
```

## 4. Erros sintáticos e recuperação

Cada diagnóstico informa **linha, coluna, o que era esperado e o que foi
encontrado**, e mostra a linha do código com um marcador na posição do erro:

```
$ python parser.py testes-parser-50/casos/28_par_ntese_de_condi_o_ausente/codigo.c
Erro de sintaxe na linha 1, coluna 24: esperado ')' para fechar a condicao do 'if', mas foi encontrado '{'
     1 | int main() { if (x > 0 { return 1; } }
       |                        ^
Entrada rejeitada: 1 erro(s) encontrado(s); nenhuma AST foi gerada.
```

Formatos de mensagem:

- `esperado X, mas foi encontrado Y` — o token atual não pode continuar a
  construção (ex.: `';' apos a declaracao de 'x'`, `expressao (identificador, literal ou '(')`);
- `token Y inesperado: motivo` — o token não pode aparecer ali de forma
  alguma (`else` sem `if`, `}` sem bloco aberto, declaração de função dentro
  de bloco).

**Recuperação (modo pânico).** O parser não para no primeiro erro:

1. dentro de um bloco, descarta tokens até um `;` (consumido) ou até o `}`
   do mesmo nível de chaves (não consumido, para fechar o bloco) e continua
   no próximo comando;
2. no nível global, descarta o restante da declaração: até um `;` fora de
   chaves ou até a `}` que fecha o corpo que começou nela;
3. para evitar mensagens em cascata, um novo erro só é relatado depois que o
   parser consumir pelo menos 3 tokens normalmente desde o erro anterior (a
   mesma heurística do yacc, citado na Aula 12).

Exemplo com dois erros independentes no mesmo arquivo:

```
Erro de sintaxe na linha 1, coluna 13: esperado ',' ou ')' na lista de parametros de 'f', mas foi encontrado identificador 'b'
     1 | int f(int a b) { return a; }
       |             ^
Erro de sintaxe na linha 2, coluna 13: esperado ',' ou ')' na lista de parametros de 'g', mas foi encontrado '{'
     2 | int g(int a {
       |             ^
Entrada rejeitada: 2 erro(s) encontrado(s); nenhuma AST foi gerada.
```

Quando há qualquer erro (léxico ou sintático), nenhuma AST é impressa.

As mensagens não usam acentos, seguindo o padrão das mensagens do scanner
da Etapa 1. O texto `Erro de sintaxe` é o que os scripts do professor
procuram para contar os erros sintáticos.

## 5. Parte C — testes

### 5.1 Scripts do professor

`src/testar_parser_python.sh` e `src/testar_parser_c.sh` são os scripts
fornecidos, copiados **sem alterações**, no mesmo diretório do scanner e dos
testes, como pedido:

```bash
cd src
bash testar_parser_python.sh ./testes-parser-50 ./parser.py
bash testar_parser_c.sh ./testes-parser-50 ./parser.c
```

Resultado (idêntico nas duas versões; logs em `testes/resultados/`):

```
Total de casos:       50
Aprovados:            16
Reprovados:           34
Erros sintáticos:     25
Erros de execução:    0
```

Os 34 casos reprovados pelo script se explicam por três motivos, nenhum
deles de estrutura da AST ou de aceitação/rejeição. O script compara a saída
**caractere por caractere** com `ast.esperada.txt`:

1. **25 casos inválidos (26–50).** Para eles, `ast.esperada.txt` contém
   apenas a frase `NÃO HÁ AST: o parser deve rejeitar a entrada.`. O README
   do próprio pacote pede, nesses casos, "código de saída diferente de zero e
   mensagem de erro sintático", e o script junta o stderr na comparação.
   Um parser que imprime a mensagem de erro exigida nunca é igual a essa
   frase. O próprio script confirma a rejeição correta: **Erros sintáticos: 25**.
2. **8 casos válidos (02, 03, 04, 06, 07, 08, 09, 10).** A AST é a mesma;
   muda só o espaçamento. Os arquivos esperados não seguem uma regra única
   (ex.: o caso 07 traz `Assign(Id(x), Lit(int,7))` e o caso 08 traz
   `Assign(Id(x),Lit(int,2))`; o caso 02 traz `int x = Lit(...)` e o 10 traz
   `int i=Lit(...)`). Uma impressão determinística, como a Aula 12 recomenda,
   não consegue coincidir com todos. A regra adotada (seção 3) coincide com
   os casos 01, 05, 11–23 e 25.
3. **Caso 24.** O `ast.esperada.txt` tem 23 `(` e 24 `)`: há um `)` a mais
   depois do `Block(...)` do `while`. Nenhuma AST bem formada produz esse
   texto. A saída do parser é a esperada sem esse parêntese extra.

O log com `KEEP_TMP=1` (`testes/resultados/log-professor-parser-py-diffs.txt`)
mostra o `diff` de cada caso e permite conferir os três motivos.

### 5.2 Verificação com os critérios do README dos testes

`src/verificar_parser.sh` aplica os critérios escritos no README do pacote
`testes-parser-50`: nos válidos, código 0 e AST igual à esperada ignorando
espaços; nos inválidos, código ≠ 0, mensagem `Erro de sintaxe` e stdout vazio.
Ele também confere se os gabaritos têm parênteses balanceados e roda os 5
programas completos da Etapa 1 (`testes/casos-programas-c`), que são válidos
e devem ser aceitos.

```bash
cd src
bash verificar_parser.sh ./testes-parser-50 ./parser.py
bash verificar_parser.sh ./testes-parser-50 ./parser.c
```

Resultado (idêntico nas duas versões):

```
Casos:                         50
Aprovados:                     49  (dos validos, 8 diferem do esperado so no espacamento)
Gabarito invalido:             1
Reprovados:                    0
Programas da Etapa 1 aceitos:  5 de 5
```

Nos 25 casos inválidos, o erro é apontado no ponto indicado pela pista de
cada caso (`resultado.esperado.txt`). Por exemplo: caso 41
(`a[1 = 2;`) → `esperado ']'` na coluna do `=`; caso 43
(`while (x < 2) }`) → `esperado inicio de comando (statement)`; caso 44 →
`token 'else' inesperado`; caso 49 → `token '}' inesperado`.

### 5.3 Cobertura pedida na Parte C

| Pedido | Casos |
|---|---|
| declaração e atribuição | 01, 02, 07, 22 |
| precedência em `a + b * c` | 03, 12, 18 |
| `if-else`, `while`, `return`, chamada | 08, 09, 10, 20, 21, 14, 15 |
| pelo menos três entradas inválidas | 26–50 (25 casos) |
| programa integrado | 25 e os 5 programas da Etapa 1 |

## 6. Robustez

Além dos testes acima, as duas versões foram submetidas a 600 entradas
geradas aleatoriamente (sequências de tokens e mutações dos casos de teste):
nenhuma travou, entrou em laço ou divergiu da outra. A versão em C foi
executada com AddressSanitizer e UndefinedBehaviorSanitizer sem nenhum
alerta, e compila sem avisos com `gcc -Wall -Wextra -std=c11`. Na versão
Python, o limite de recursão é ampliado para suportar expressões com
centenas de níveis de parênteses.

## 7. Paridade entre Python e C

As duas versões foram comparadas com as três formas de saída (padrão,
`--arvore` e `--tokens`) sobre os 50 casos, os programas e exemplos da
Etapa 1 e os casos léxicos válidos e inválidos: stdout, stderr e código de
saída idênticos em todas as 216 execuções.
