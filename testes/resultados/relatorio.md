# Resultado dos testes do scanner MINIC

## Casos validos (.minic)

| Caso | Resultado | Tokens | Erros | Observacao |
|---|---|---|---|---|
| `v01_declaracoes.minic` | OK | 27 | 0 |  |
| `v02_expressao_precedencia.minic` | OK | 31 | 0 |  |
| `v03_operadores_compostos.minic` | OK | 31 | 0 |  |
| `v04_comentarios_e_posicoes.minic` | OK | 12 | 0 |  |
| `v05_funcoes_e_vetores.minic` | OK | 48 | 0 |  |
| `v06_reservadas_vs_identificadores.minic` | OK | 21 | 0 |  |
| `v07_literais_opcionais.minic` | OK | 16 | 0 |  |

## Casos invalidos (.minic)

| Caso | Resultado | Tokens | Erros | Observacao |
|---|---|---|---|---|
| `i01_simbolo_desconhecido.minic` | OK | 6 | 1 |  |
| `i02_comentario_nao_terminado.minic` | OK | 6 | 1 |  |
| `i03_caractere_nao_terminado.minic` | OK | 9 | 1 |  |
| `i04_cadeia_nao_terminada.minic` | OK | 5 | 1 |  |
| `i05_numero_real_malformado.minic` | OK | 6 | 1 |  |
| `i06_identificador_iniciado_por_digito.minic` | OK | 7 | 1 |  |
| `i07_operador_logico_incompleto.minic` | OK | 12 | 2 |  |

## Programas completos em C (.c)

| Caso | Resultado | Tokens | Erros | Observacao |
|---|---|---|---|---|
| `c01_fibonacci.c` | OK | 85 | 0 |  |
| `c02_primos.c` | OK | 96 | 0 |  |
| `c03_media_vetor.c` | OK | 115 | 0 |  |
| `c04_menu_interativo.c` | OK | 95 | 0 |  |
| `c05_controle_temperatura.c` | OK | 61 | 0 |  |

**Resumo: 19/19 casos passaram.**
