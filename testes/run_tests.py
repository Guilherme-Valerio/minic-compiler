#!/usr/bin/env python3
"""
run_tests.py - Executa o scanner sobre todos os casos de teste e compara
o resultado com os arquivos *.expected.jsonl / *.errors.jsonl.

Uso:
    python testes/run_tests.py

Gera um relatorio em testes/resultados/relatorio.md e imprime um resumo
no terminal. O codigo de saida e 0 se todos os casos passarem, 1 caso
contrario (util para usar em CI).
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))

from scanner import tokenize  # noqa: E402

TESTES_DIR = Path(__file__).resolve().parent
RESULTADOS_DIR = TESTES_DIR / "resultados"


def read_jsonl(path: Path):
    if not path.exists():
        return []
    items = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            items.append(json.loads(line))
    return items


def tokens_to_dicts(tokens):
    return [t.to_dict() for t in tokens]


def errors_to_dicts(errors):
    return [e.to_dict() for e in errors]


def compare(actual, expected):
    """Retorna None se identicos, ou uma string descrevendo a 1a diferenca."""
    if actual == expected:
        return None
    for i, (a, e) in enumerate(zip(actual, expected)):
        if a != e:
            return f"posicao {i}: obtido {a} | esperado {e}"
    if len(actual) != len(expected):
        return (
            f"quantidade de itens diferente: obtido {len(actual)} | "
            f"esperado {len(expected)}"
        )
    return "diferenca desconhecida"


def run_case(minic_path: Path, expected_path: Path, errors_path: Path = None):
    source = minic_path.read_text(encoding="utf-8")
    tokens, errors = tokenize(source)

    actual_tokens = tokens_to_dicts(tokens)
    expected_tokens = read_jsonl(expected_path)
    tokens_diff = compare(actual_tokens, expected_tokens)

    errors_diff = None
    if errors_path is not None:
        actual_errors = errors_to_dicts(errors)
        expected_errors = read_jsonl(errors_path)
        errors_diff = compare(actual_errors, expected_errors)

    ok = tokens_diff is None and errors_diff is None
    return ok, tokens_diff, errors_diff, len(tokens), len(errors)


def find_cases(directory: Path, with_errors: bool):
    cases = []
    if not directory.exists():
        return cases
    for minic in sorted(directory.glob("*.minic")):
        expected = minic.with_suffix("").with_suffix(".expected.jsonl") \
            if False else Path(str(minic)[: -len(".minic")] + ".expected.jsonl")
        errors = Path(str(minic)[: -len(".minic")] + ".errors.jsonl") if with_errors else None
        cases.append((minic, expected, errors))
    for c_file in sorted(directory.glob("*.c")):
        expected = Path(str(c_file) + ".expected.jsonl")
        cases.append((c_file, expected, None))
    return cases


def main():
    grupos = [
        ("Casos validos (.minic)", find_cases(TESTES_DIR / "casos-validos", with_errors=False)),
        ("Casos invalidos (.minic)", find_cases(TESTES_DIR / "casos-invalidos", with_errors=True)),
        ("Programas completos em C (.c)", find_cases(TESTES_DIR / "casos-programas-c", with_errors=False)),
    ]

    linhas_relatorio = ["# Resultado dos testes do scanner MINIC", ""]
    total = 0
    total_ok = 0

    for titulo, casos in grupos:
        linhas_relatorio.append(f"## {titulo}")
        linhas_relatorio.append("")
        linhas_relatorio.append("| Caso | Resultado | Tokens | Erros | Observacao |")
        linhas_relatorio.append("|---|---|---|---|---|")
        print(f"\n{titulo}")
        print("-" * len(titulo))

        for minic_path, expected_path, errors_path in casos:
            total += 1
            ok, tokens_diff, errors_diff, n_tokens, n_errors = run_case(
                minic_path, expected_path, errors_path
            )
            nome = minic_path.name
            if ok:
                total_ok += 1
                status = "OK"
                obs = ""
                print(f"  [OK]   {nome}")
            else:
                status = "FALHOU"
                obs_parts = []
                if tokens_diff:
                    obs_parts.append(f"tokens: {tokens_diff}")
                if errors_diff:
                    obs_parts.append(f"erros: {errors_diff}")
                obs = "; ".join(obs_parts)
                print(f"  [FALHOU] {nome} -> {obs}")

            linhas_relatorio.append(
                f"| `{nome}` | {status} | {n_tokens} | {n_errors} | {obs} |"
            )
        linhas_relatorio.append("")

    resumo = f"**Resumo: {total_ok}/{total} casos passaram.**"
    linhas_relatorio.append(resumo)
    print(f"\n{resumo}")

    RESULTADOS_DIR.mkdir(exist_ok=True)
    relatorio_path = RESULTADOS_DIR / "relatorio.md"
    relatorio_path.write_text("\n".join(linhas_relatorio) + "\n", encoding="utf-8")
    print(f"\nRelatorio salvo em: {relatorio_path.relative_to(ROOT)}")

    return 0 if total_ok == total else 1


if __name__ == "__main__":
    sys.exit(main())
