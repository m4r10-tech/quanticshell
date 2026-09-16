#!/bin/bash
# Quantic Shell - suite de tests automatizados.
# Uso: ./tests/run.sh  (o `make test`)
# Aísla HOME en un temporal para no tocar ~/.quanticshell real.

set -u

QSH="$(pwd)/quanticshell"
TMPBASE="$(mktemp -d)"
export HOME="$TMPBASE/home"
mkdir -p "$HOME"
WORK="$TMPBASE/work"
mkdir -p "$WORK"

PASS=0
FAIL=0
FAILED_CASES=()

# assert_eq "descripcion" "esperado" "obtenido"
assert_eq() {
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED_CASES+=("$1 (esperado: [$2], obtenido: [$3])")
    fi
}

# run "comando" -> stdout exacto, sin prompts (modo no interactivo)
run() {
    "$QSH" -c "$1" 2>&1
}

echo "=== Quantic Shell - tests ==="

# --- compilación limpia ---
BUILD_OUT="$(make 2>&1)"
assert_eq "compila sin errores" "0" "$?"
echo "$BUILD_OUT" | grep -qiE "warning|error" && {
    FAIL=$((FAIL + 1)); FAILED_CASES+=("compilación con warnings: $BUILD_OUT")
} || PASS=$((PASS + 1))

# --- builtins básicos ---
assert_eq "echo" "hola mundo" "$(run 'echo hola mundo')"
assert_eq "echo -n" "sin salto" "$(run 'echo -n sin salto')"
assert_eq "pwd" "$WORK" "$(cd "$WORK" && run 'pwd')"
assert_eq "cd + pwd" "/tmp" "$(run 'cd /tmp; pwd')"
assert_eq "cd inexistente" "cd: no existe '/no/existe'" "$(run 'cd /no/existe')"
assert_eq "help menciona jobs" "1" "$(run 'help' | grep -c jobs)"

# --- variables y expansiones ---
assert_eq "export + \$VAR" "123" "$(run 'export TV=123; echo $TV')"
assert_eq "\${VAR}" "abc" "$(run 'export W=abc; echo ${W}')"
assert_eq '$? tras ok' "0" "$(run 'true; echo $?')"
assert_eq '$? tras fallo' "1" "$(run 'false; echo $?')"
assert_eq "unset" "" "$(run 'export U=1; unset U; echo $U')"
assert_eq "comillas dobles expanden" "dq /x" "$(run 'export H=/x; echo "dq $H"')"
assert_eq "comillas simples no expanden" 'sq $H' "$(run "export H=/x; echo 'sq \$H'")"

# --- exit codes ---
run 'exit 7' >/dev/null 2>&1
assert_eq "exit N" "7" "$?"
run 'false' >/dev/null 2>&1
assert_eq "exit de externo fallido" "1" "$?"

# --- pipes / redirecciones / glob (ejecutor nativo) ---
assert_eq "pipe" "B" "$(run 'echo b | tr a-z A-Z')"
assert_eq "redir > y cat <" "hola" "$(run "echo hola > $WORK/f.txt; cat < $WORK/f.txt")"
assert_eq "redir >>" "hola2" "$(run "echo hola > $WORK/g.txt; echo 2 >> $WORK/g.txt; tr -d '\n' < $WORK/g.txt")"
touch "$WORK/a.c" "$WORK/b.c"
assert_eq "glob" "2" "$(run "cd $WORK; echo *.c | wc -w")"

# --- pipeline status = último comando ---
assert_eq "false|true -> 0" "0" "$(run 'false | true; echo $?')"
assert_eq "true|false -> 1" "1" "$(run 'true | false; echo $?')"

# --- encadenamiento y fallback sh ---
assert_eq "punto y coma" "uno dos" "$(run 'echo -n uno; echo " dos"' | tr -d '\n')"
assert_eq "fallback \$()" "sub" "$(run 'echo $(echo sub)')"
assert_eq "fallback &&" "ok" "$(run 'true && echo ok')"

# --- alias (incluye sintaxis =) ---
assert_eq "alias crear+usar" "alias_ok" "$(run 'alias ll echo alias_ok >/dev/null; ll; unalias ll')"
run "alias gs='git status'" >/dev/null
assert_eq "alias con =" "git status" "$(run "alias gs" | sed 's/.*-> //')"
run "unalias gs" >/dev/null

# --- jobs ---
OUT_JOBS="$(run 'sleep 0.05 &; jobs >/dev/null; wait; echo done')"
assert_eq "background+jobs+wait termina" "done" "$(printf '%s\n' "$OUT_JOBS" | tail -n 1)"
assert_eq "background avisa [id]" "1" "$(printf '%s\n' "$OUT_JOBS" | grep -c '^\[[0-9]*\]' || true)"
# El aviso "[1] pid" al lanzar es correcto (como bash); jobs debe quedar vacío.
assert_eq "jobs vacío tras wait" "0" "$(run 'sleep 0.05 &; wait; jobs' | grep -c Ejecutando || true)"
# Redirigir un builtin no debe tragarse salida anterior pendiente en el buffer.
OUT_BUF="$(run 'sleep 0.05 &; jobs >/dev/null; wait; echo done')"
assert_eq "redir builtin no roba buffer" "done" "$(printf '%s\n' "$OUT_BUF" | tail -n 1)"
assert_eq "redir builtin conserva aviso" "1" "$(printf '%s\n' "$OUT_BUF" | grep -c '^\[[0-9]*\]' || true)"

# --- source y rc ---
printf 'echo desde_rc\n' > "$WORK/mini.rc"
assert_eq "source" "desde_rc" "$(run "source $WORK/mini.rc")"

# --- salida limpia no interactiva ---
assert_eq "pipe sin banner/prompts" "solo_esto" "$(printf 'echo solo_esto\n' | "$QSH" 2>&1)"
assert_eq "sin ANSI por pipe" "0" "$(printf 'echo x\n' | "$QSH" 2>&1 | grep -c $'\x1b' || true)"

# --- flags ---
assert_eq "--version" "quanticshell 2.0.0" "$("$QSH" --version)"
assert_eq "-c" "C_OK" "$(run 'echo C_OK')"

# --- persistencia aislada (no toca la real) ---
assert_eq "aliases no fugan al HOME real" "0" "$(grep -q alias_ok ~/.quanticshell/aliases 2>/dev/null && echo 1 || echo 0)"

echo "-------------------------------"
echo "PASS: $PASS  FAIL: $FAIL"
if [ "$FAIL" -ne 0 ]; then
    for c in "${FAILED_CASES[@]}"; do echo "  FALLA: $c"; done
fi

rm -rf "$TMPBASE"
[ "$FAIL" -eq 0 ]
