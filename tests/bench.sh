#!/bin/bash
# Quantic Shell - benchmarks de velocidad.
# Uso: ./tests/bench.sh  (o `make bench`)
# Compara arranque y throughput contra bash/dash si existen.
# Todo ruido de fork/exec del propio loop afecta a todos por igual.

set -u
export LC_ALL=C # decimales con punto para awk

QSH="$(pwd)/quanticshell"
[ -x "$QSH" ] || { echo "falta $QSH (corre make primero)"; exit 1; }

N_START=200
N_ECHO=200
CADENA=$(python3 -c "print(';'.join(['echo a']*40))")

tiempo() { # tiempo <repeticiones> <comando...>: imprime segundos totales
    local n="$1"; shift
    local inicio fin
    inicio=$(date +%s%N)
    for _ in $(seq 1 "$n"); do "$@" >/dev/null 2>&1; done
    fin=$(date +%s%N)
    awk "BEGIN {printf \"%.3f\", ($fin - $inicio) / 1e9}"
}

fila() { # fila <nombre> <tiempo_total> <n>: imprime nombre, total y ms/invocación
    local ms
    ms=$(awk "BEGIN {printf \"%.2f\", $2 * 1000 / $3}")
    printf "%-28s %8ss total   %7s ms/inv\n" "$1" "$2" "$ms"
}

echo "=== Quantic Shell - bench (vs bash/dash si existen) ==="
echo "--- arranque: -c exit (bash/dash: -c exit), N=$N_START ---"
fila "quanticshell" "$(tiempo "$N_START" "$QSH" -c exit)" "$N_START"
command -v dash >/dev/null && fila "dash" "$(tiempo "$N_START" dash -c exit)" "$N_START"
command -v bash >/dev/null && fila "bash" "$(tiempo "$N_START" bash -c exit)" "$N_START"

echo "--- throughput: 40 builtins/invocación, N=$N_ECHO ---"
fila "quanticshell (echo nativo)" "$(tiempo "$N_ECHO" "$QSH" -c "$CADENA")" "$N_ECHO"
command -v bash >/dev/null && fila "bash (echo builtin)" "$(tiempo "$N_ECHO" bash -c "$CADENA")" "$N_ECHO"

echo "--- binario ---"
ls -la "$QSH" | awk '{printf "tamaño: %s bytes\n", $5}'
command -v bash >/dev/null && ls -la "$(command -v bash)" | awk '{printf "bash:     %s bytes\n", $5}'
