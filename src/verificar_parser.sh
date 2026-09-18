#!/usr/bin/env bash
# verificar_parser.sh - verifica o parser com os criterios descritos no
# README.md de testes-parser-50 (escrito pelo professor):
#
#   casos validos (01-25):   codigo de saida 0 e AST igual a ast.esperada.txt
#                            (a comparacao ignora espacos, que nao fazem parte
#                            da estrutura da arvore);
#   casos invalidos (26-50): codigo de saida diferente de zero, mensagem de
#                            erro sintatico e nenhuma AST no stdout.
#
# Alem disso, confere se os ast.esperada.txt tem parenteses balanceados e
# roda os programas completos da Etapa 1 (testes/casos-programas-c), que
# sao programas validos e devem ser aceitos.
#
# Uso (no diretorio src/):
#   bash verificar_parser.sh ./testes-parser-50 ./parser.py
#   bash verificar_parser.sh ./testes-parser-50 ./parser.c

set -u
TEST_DIR="${1:-./testes-parser-50}"
PARSER="${2:-./parser.py}"

case "$PARSER" in
    *.py) RUN=(python3 "$PARSER") ;;
    *.c)  BIN="${PARSER%.c}"
          gcc -Wall -Wextra -std=c11 "$PARSER" -o "$BIN" || { echo "ERRO: compilacao falhou"; exit 2; }
          RUN=("$BIN") ;;
    *)    RUN=("$PARSER") ;;
esac

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0; GAB=0; ESPACO=0; TOTAL=0

balanced() { local s; s="$(cat "$1")"; local o="${s//[^(]/}" c="${s//[^)]/}"; [[ ${#o} -eq ${#c} ]]; }

printf 'Parser: %s\nCasos: %s\n\n' "$PARSER" "$TEST_DIR"
for dir in $(find "$TEST_DIR" -name codigo.c -printf '%h\n' | sort); do
    TOTAL=$((TOTAL + 1)); name="$(basename "$dir")"
    "${RUN[@]}" "$dir/codigo.c" >"$TMP/out" 2>"$TMP/err"; status=$?
    status_esperado="$(head -n1 "$dir/resultado.esperado.txt" | tr -d '\r')"

    if [[ "$status_esperado" == "ACEITO" ]]; then
        exp="$(tr -d '[:space:]' < "$dir/ast.esperada.txt")"
        got="$(tr -d '[:space:]' < "$TMP/out")"
        if [[ $status -eq 0 && "$exp" == "$got" ]]; then
            PASS=$((PASS + 1)); nota=""
            if ! cmp -s <(tr -d '\r' < "$dir/ast.esperada.txt" | sed 's/[[:space:]]*$//') "$TMP/out"; then
                ESPACO=$((ESPACO + 1)); nota="  (difere do esperado so no espacamento)"
            fi
            printf '%-46s OK%s\n' "$name" "$nota"
        elif [[ $status -eq 0 ]] && ! balanced "$dir/ast.esperada.txt"; then
            GAB=$((GAB + 1))
            printf '%-46s GABARITO INVALIDO: ast.esperada.txt tem parenteses desbalanceados\n' "$name"
            printf '%-46s   esperado: %s\n%-46s   obtido:   %s\n' "" "$(cat "$dir/ast.esperada.txt")" "" "$(cat "$TMP/out")"
        else
            FAIL=$((FAIL + 1)); printf '%-46s FALHOU (status=%d)\n' "$name" "$status"
        fi
    else
        if [[ $status -ne 0 && ! -s "$TMP/out" ]] && grep -qi 'erro de sintaxe' "$TMP/err"; then
            PASS=$((PASS + 1)); printf '%-46s OK  -> %s\n' "$name" "$(head -n1 "$TMP/err" | cut -c1-110)"
        else
            FAIL=$((FAIL + 1)); printf '%-46s FALHOU (status=%d, esperado rejeicao com erro de sintaxe)\n' "$name" "$status"
        fi
    fi
done

EXTRA_OK=0; EXTRA_TOTAL=0
PROGS="$(dirname "$0")/../testes/casos-programas-c"
if [[ -d "$PROGS" ]]; then
    printf '\nProgramas completos da Etapa 1 (devem ser aceitos)\n'
    for f in "$PROGS"/*.c; do
        EXTRA_TOTAL=$((EXTRA_TOTAL + 1))
        if "${RUN[@]}" "$f" >/dev/null 2>&1; then EXTRA_OK=$((EXTRA_OK + 1)); r=ACEITO; else r="REJEITADO (erro)"; fi
        printf '%-46s %s\n' "$(basename "$f")" "$r"
    done
fi

printf '\nResumo\n------\n'
printf 'Casos:                         %d\n' "$TOTAL"
printf 'Aprovados:                     %d  (dos validos, %d diferem do esperado so no espacamento)\n' "$PASS" "$ESPACO"
printf 'Gabarito invalido:             %d\n' "$GAB"
printf 'Reprovados:                    %d\n' "$FAIL"
printf 'Programas da Etapa 1 aceitos:  %d de %d\n' "$EXTRA_OK" "$EXTRA_TOTAL"
[[ $FAIL -eq 0 && $EXTRA_OK -eq $EXTRA_TOTAL ]]
