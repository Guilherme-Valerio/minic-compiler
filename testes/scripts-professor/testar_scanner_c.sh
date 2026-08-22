#!/usr/bin/env bash
# Testa um analisador léxico escrito em C com arquivos .c e .minic.
#
# Uso:
#   ./testar_scanner_c.sh [scanner.c] [pasta-de-testes]
#
# O scanner deve aceitar:
#   ./scanner arquivo.c
#   ./scanner arquivo.minic
#
# A saída padrão deve conter um objeto JSON por linha, compatível com os
# arquivos <entrada>.expected.jsonl. Diagnósticos esperados podem ser
# armazenados em <entrada>.errors.jsonl.


SCANNER_SOURCE="${1:-scanner.c}"
TESTS_DIR="${2:-.}"
SCANNER_BINARY="./scanner"
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0

compare_jsonl() {
    local actual="$1" expected_tokens="$2"
    python3 - "$actual" "$expected_tokens" <<'PY'
import json
import sys
from pathlib import Path

actual_path, expected_tokens_path = sys.argv[1:]

def read_jsonl(path):
    values = []
    if not Path(path).exists():
        return values
    for number, line in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip():
            continue
        try:
            values.append(json.loads(line))
        except json.JSONDecodeError as exc:
            print(f'FALHA: JSON inválido em {path}:{number}: {exc}')
            raise SystemExit(1)
    return values

def first_difference(got, want):
    for i in range(max(len(got), len(want))):
        a = got[i] if i < len(got) else '<ausente>'
        b = want[i] if i < len(want) else '<a mais>'
        if a != b:
            print(f'  primeira diferença na posição {i + 1}:')
            print(f'    esperado: {json.dumps(b, ensure_ascii=False)}')
            print(f'    obtido:   {json.dumps(a, ensure_ascii=False)}')
            return

produced = read_jsonl(actual_path)
expected_tokens = read_jsonl(expected_tokens_path)
produced_tokens = [x for x in produced if 'token' in x]
ok = True

if produced_tokens != expected_tokens:
    print(f'FALHA: sequência de tokens diferente.')
    first_difference(produced_tokens, expected_tokens)
    ok = False
if ok:
    print(f'OK: {len(produced_tokens)} token(s).')
raise SystemExit(0 if ok else 1)
PY
}

run_case() {
    local input="$1" expected="$2" label="${1#./}"
    TOTAL=$((TOTAL + 1))
    printf '\n================================================================\nCaso: %s\n' "$label"
    printf 'Comando: %s %s\n' "$SCANNER_BINARY" "$input"
    printf 'Resultado esperado: %s\n' "$expected"

    set +e
    "$SCANNER_BINARY" "$input" > output.jsonl
    status=$?
    set -e

    if compare_jsonl output.jsonl "$expected"; then
        echo 'RESULTADO: OK'
        PASS=$((PASS + 1))
    else
        echo 'RESULTADO: FALHOU — tokens diferentes do esperado'
        FAIL=$((FAIL + 1))
    fi
    if (( status != 0 )); then
        WARN=$((WARN + 1))
        echo "Aviso: o scanner terminou com código $status."
    fi
}

printf '%s\n' '== Compilando o analisador léxico =='
gcc -Wall -Wextra -std=c11 "$SCANNER_SOURCE" -o "$SCANNER_BINARY" || fail 'a compilação falhou.'
printf 'Executável gerado: %s\n' "$SCANNER_BINARY"

mapfile -t expected_files < <(find "$TESTS_DIR" -type f -name '*.expected.jsonl' -print | sort)
(( ${#expected_files[@]} > 0 )) || { echo "ERRO: nenhum resultado esperado para .c ou .minic foi encontrado." >&2; exit 2; }

for expected in "${expected_files[@]}"; do
    input="${expected%.expected.jsonl}"
    if [[ -f "$input" || -f "$input.minic" ]]; then
        if [[ -f "$input" ]]; then
            run_case "$input" "$expected"
        fi
        if [[ -f "$input.minic" ]]; then
            run_case "$input.minic" "$expected"
        fi
    else
        echo "AVISO: entrada correspondente não encontrada para $expected" >&2
        WARN=$((WARN + 1))
    fi
done

printf '\n================================================================\n'
printf 'Resumo: %d OK, %d falharam, %d avisos, %d casos verificados.\n' "$PASS" "$FAIL" "$WARN" "$TOTAL"
(( FAIL == 0 && WARN == 0 ))
