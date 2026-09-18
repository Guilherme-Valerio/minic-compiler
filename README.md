# Compilador MINIC — Etapa 1 (léxico) e Etapa 2 (sintático)

Projeto da disciplina de Compiladores — **Etapa 1: análise léxica** (item
**a — Tokens**) do Projeto MINIC. Implementa um scanner que lê um programa
MINIC (subconjunto de C) e produz a sequência de tokens reconhecidos, com
diagnóstico de erros léxicos. O scanner foi escrito em **Python e em C**,
com exatamente a mesma especificação léxica e as mesmas regras de
recuperação de erro nas duas versões (ver
[`docs/especificacao-lexica.md`](docs/especificacao-lexica.md)).

## Integrantes
Alberto Dias - RA:2303748
Guilherme Valerio - RA: 2401213
Victor Marques - RA: 2401270
Vitor Siqueira - RA: 2302346

## Etapa 2: analisador sintático (parser)

O parser recebe os tokens do scanner da Etapa 1 (que não foi alterado),
verifica a estrutura do programa pela gramática da MINIC e constrói a AST.
Foi escrito em **Python e em C**, com a mesma gramática, a mesma AST e as
mesmas mensagens de erro: as duas versões produzem saída idêntica, byte a
byte. Nas duas, a saída do scanner da Etapa 1 alimenta o parser: o
`parser.py` importa o `scanner.py`, e o `parser.c` inclui o `scanner.c` e lê
a saída JSON Lines que ele produz. Gramática, contrato scanner→parser, formato da AST, erros e testes
estão em [`docs/especificacao-sintatica.md`](docs/especificacao-sintatica.md).

```bash
cd src
python parser.py codigo.c                  # Python
gcc -Wall -Wextra -std=c11 parser.c -o parser
./parser codigo.c                          # C
```

- **entrada aceita:** AST no stdout, código de saída 0;
- **entrada rejeitada:** diagnósticos no stderr (linha, coluna, o que era
  esperado e o que foi encontrado, com a linha do código marcada), nenhuma
  AST, código de saída 1. O parser se recupera de erros e relata mais de um
  erro por arquivo quando existem.

```
$ python parser.py ../testes/exemplo-aula/exemplo.c
Program(Function(int main() Block(VarDecl(int total=Binary(+,Lit(int,2),Binary(*,Lit(int,3),Lit(int,4)))), ExprStmt(Call(Id(print),Id(total))), Return(Lit(int,0)))))
```

Opções extras: `--arvore` (AST indentada, um nó por linha) e `--tokens`
(lista também os tokens do scanner).

### Testes da Etapa 2

Os scripts do professor e os 50 casos estão em `src/`, junto do scanner,
como pedido. Os scripts foram copiados sem alterações:

```bash
cd src
bash testar_parser_python.sh ./testes-parser-50 ./parser.py
bash testar_parser_c.sh ./testes-parser-50 ./parser.c
```

Esses scripts comparam a saída caractere por caractere com
`ast.esperada.txt` e reportam **16/50** nas duas versões. As 34 diferenças
não são de estrutura da AST nem de aceitação/rejeição, e estão explicadas
com os `diff`s na seção 5.1 da especificação sintática:

- nos 25 casos inválidos, o esperado é só a frase "NÃO HÁ AST", mas o
  README dos testes exige mensagem de erro sintático (o próprio script
  conta **25 erros sintáticos** detectados);
- em 8 casos válidos a AST é igual e só o espaçamento difere, porque os
  arquivos esperados não seguem um espaçamento único;
- o `ast.esperada.txt` do caso 24 tem um `)` a mais (23 abertos, 24 fechados).

O script `src/verificar_parser.sh` aplica os critérios escritos no README
do pacote de testes (AST igual ignorando espaços; rejeição com código ≠ 0 e
mensagem de erro sintático) e confirma **49 aprovados, 0 reprovados e 1
gabarito inválido (caso 24)** nas duas versões, além dos 5 programas
completos da Etapa 1 aceitos:

```bash
bash verificar_parser.sh ./testes-parser-50 ./parser.py
bash verificar_parser.sh ./testes-parser-50 ./parser.c
```

Logs de todas as execuções em `testes/resultados/`.

## Estrutura do repositório

```
minic-lexer/
├── README.md                       este arquivo
├── docs/
│   ├── especificacao-lexica.md     tokens, expressões regulares e autômatos
│   └── especificacao-sintatica.md  gramática, AST, erros e testes do parser
├── src/
│   ├── scanner.py                  scanner em Python (biblioteca + CLI)
│   ├── scanner.c                   scanner em C (mesma especificação)
│   ├── parser.py                   parser em Python (usa scanner.py)
│   ├── parser.c                    parser em C (mesma gramática e saída)
│   ├── testar_parser_python.sh     script de teste do professor (Etapa 2)
│   ├── testar_parser_c.sh          script de teste do professor (Etapa 2)
│   ├── verificar_parser.sh         verificação com os critérios do README dos testes
│   └── testes-parser-50/           50 casos do parser fornecidos pelo professor
└── testes/
    ├── run_tests.py                roda os 19 casos e gera o relatório
    ├── scripts-professor/          scripts de teste fornecidos pelo professor
    ├── casos-validos/              7 casos de entrada válida (v01–v07)
    ├── casos-invalidos/            7 casos de erro léxico (i01–i07)
    ├── casos-programas-c/          5 programas completos (fibonacci, primos, ...)
    ├── exemplo-aula/               exemplo do slide da aula (limpo e com erros)
    └── resultados/                 saída gerada pelos testes (relatórios + logs)
```

## Como rodar

Requer Python 3 e, para a versão em C, um compilador C (`gcc`). Nenhuma
dependência externa além disso.

```bash
# Python: saída em JSON Lines no stdout (padrão), 1 token por linha
python src/scanner.py testes/exemplo-aula/exemplo.c

# Python: tabela legível no terminal
python src/scanner.py testes/exemplo-aula/exemplo.c --format text

# C: compilar e rodar
gcc -std=c11 -O2 src/scanner.c -o src/scanner
./src/scanner testes/exemplo-aula/exemplo.c

# rodar toda a suíte de testes (compara as duas versões com os fixtures)
python testes/run_tests.py
```

O `run_tests.py` roda o **scanner em Python** sobre os 19 casos e compara
com `*.expected.jsonl` / `*.errors.jsonl`, gravando o resultado em
`testes/resultados/relatorio.md`: **19/19 casos passando**. O scanner em
C foi validado separadamente com o script de teste do professor (ver
próxima seção) e produz, para os mesmos 19 casos, uma saída idêntica,
byte a byte, à do scanner em Python.

## Scripts de teste do professor

Os dois scripts fornecidos em aula (`testar_scanner_py.sh` para a versão
Python e `testar_scanner_c.sh` para a versão C, ambos em
`testes/scripts-professor/`, copiados sem alterações) chamam o scanner
diretamente:

```bash
python3 scanner.py arquivo.c        # deve imprimir JSON Lines no stdout
./scanner arquivo.c                 # idem, para o binário compilado em C
```

e comparam a saída com `*.expected.jsonl`. Por isso o **formato padrão da
CLI é JSON Lines** (um token por linha em stdout; diagnósticos, se houver,
em stderr) — é o que essas automações esperam ao chamar o programa sem
flags. Os dois scripts rodam localmente contra este repositório com
**19 OK, 0 falharam, 0 avisos** (logs completos salvos em
`testes/resultados/log-professor-scanner-py.txt` e
`testes/resultados/log-professor-scanner-c.txt`).

> Nota: o script `testar_scanner_c.sh`, como enviado pelo professor, chama
> uma função `fail` na etapa de compilação (`gcc ... || fail 'a compilação
> falhou.'`) que não é definida em nenhum lugar do próprio script. Isso só
> aparece se a compilação falhar (não é o nosso caso — `scanner.c` compila
> limpo com `-Wall -Wextra -std=c11`), mas vale avisar o professor caso
> outro grupo esbarre nisso.

## Formato de saída

Cada token é um objeto JSON com nome, lexema, linha, coluna e atributo,
seguindo o formato combinado em aula:

```json
{"token":"INT_LIT","lexeme":"12","attribute":12,"line":1,"column":9}
```

`EOF` é sempre o último token. Espaços, quebras de linha e comentários não
geram token, mas alteram linha/coluna. Erros léxicos são reportados
separadamente (stderr / `*.errors.jsonl`), com `error`, `lexeme`, `line` e
`column`.

Detalhes completos de tokens, padrões e autômatos estão em
[`docs/especificacao-lexica.md`](docs/especificacao-lexica.md).

## Sobre os casos de teste

Os casos em `testes/casos-invalidos` (i01–i06) e `testes/casos-programas-c`
(c01–c05) foram fornecidos pelo professor. Durante a montagem deste
repositório, alguns arquivos citados no `MANIFESTO.md` original não
estavam presentes no pacote entregue e foram completados para fechar a
cobertura do checklist:

- **`i06_...minic`**: o arquivo de entrada não veio no pacote — foi
  reconstruído a partir das posições (linha/coluna) do `expected.jsonl`
  já fornecido (`int 123abc = 4;`).
- **`i07_operador_logico_incompleto`**: citado no manifesto, mas sem
  nenhum arquivo — foi criado do zero (testa `&` e `|` isolados).
- **`casos-validos/v01`–`v07`**: a pasta inteira não veio no pacote,
  apesar de estar descrita no manifesto — foram escritos exemplos
  cobrindo exatamente o que cada `v0N` descreve (declarações,
  precedência, operadores compostos, comentários/posições, funções e
  vetores, palavras reservadas vs. identificadores parecidos, e
  literais opcionais). O `expected.jsonl` desses casos foi gerado
  rodando o próprio scanner (por isso é autoconsistente com a
  implementação, e não uma referência externa).
  

## Chamada do scanner

```bash
python scanner.py file.c      # versão Python (src/scanner.py)
./scanner file.c              # versão C, após compilar (gcc scanner.c -o scanner)
```

Saída padrão: JSON Lines no `stdout` (compatível com os scripts de teste
do professor). Use `--format text` na versão Python para uma tabela
legível no terminal.
