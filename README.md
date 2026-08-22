# Analisador léxico da MINIC (Etapa 1)

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

## Estrutura do repositório

```
minic-lexer/
├── README.md                       este arquivo
├── docs/
│   └── especificacao-lexica.md     tokens, expressões regulares e autômatos
├── src/
│   ├── scanner.py                  scanner em Python (biblioteca + CLI)
│   └── scanner.c                   scanner em C (mesma especificação)
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
