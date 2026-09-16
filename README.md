# Quantic Shell

Una shell minimalista escrita en C diseñada como proyecto educativo para aprender programación de sistemas POSIX. Implementa las funcionalidades esenciales de una shell interactiva moderna mientras practica conceptos fundamentales de C.

## Características

### Funcionalidades implementadas

- **Prompt configurable**: Personaliza nombre de usuario, host y símbolo del prompt
- **Directorio actual**: Muestra la ruta actual con acortamiento automático de `~` para el directorio home
- **Colores ANSI**: Interfaz visual con colores para distinguir diferentes partes del prompt
- **Comandos internos**: `cd`, `help`, `exit`, `config`, `alias`, `unalias`, `echo`, `pwd`, `export`, `unset`, `history`, `jobs`, `fg`, `bg`, `wait`, `source`
- **Aliases persistentes**: Crea atajos que sobreviven entre sesiones
- **Encadenamiento de comandos**: Ejecuta múltiples comandos con `;`
- **Pipes y redirecciones nativas**: `|`, `>`, `>>`, `<` con `fork`+`execvp` directo (sin `/bin/sh`, más rápido)
- **Globbing nativo**: `*.c`, `?.txt` vía `glob()`
- **Variables y expansiones**: `$VAR`, `${VAR}`, `$?`, `$$`, `~`
- **Job control**: `&`, `jobs`, `fg`, `bg`, `wait`, Ctrl+Z
- **Historial navegable**: Usa las flechas ↑/↓ para recorrer comandos anteriores
- **Edición de línea**: ←/→, Ctrl+A/E/U/K/W, Supr, Tab-completion (comandos y ficheros), Ctrl+R (búsqueda)
- **Historial persistente**: append por línea (rápido, hasta 500 entradas)
- **Prompt con git y exit code**: muestra rama `(main)` sin hacer fork y `[$?]` en rojo si falla
- **RC de arranque**: `~/.quanticshell/rc` se ejecuta al inicio (`source` disponible)
- **Manejo de señales**: Ctrl+C cancela comandos sin cerrar la shell
- **Modo interactivo y no interactivo**: Funciona tanto en terminal como con pipes
- **Salida limpia por pipe**: sin banner, sin prompts y sin ANSI (respeta `NO_COLOR`), apta para scripts: `echo hola | quanticshell` → `hola`

## Instalación

### Requisitos

- **Para usar el binario precompilado**: Linux x86_64
- **Para compilar desde fuente**: Compilador C compatible con C11 (GCC, Clang)
- **Sistema operativo**: Unix/Linux/macOS
- **Opcional**: make (para usar el Makefile)

### Opción 1: Instalación automática (recomendado)

El script de instalación detecta automáticamente tu sistema y arquitectura, compila el binario y lo instala:

```bash
# Clonar el repositorio
git clone https://github.com/m4r10-tech/quanticshell.git
cd quanticshell

# Ejecutar el instalador
chmod +x install.sh
./install.sh
```

El instalador:
- Detecta tu sistema operativo (Linux/macOS) y arquitectura (x86_64, ARM64, ARMv7, etc.)
- Compila el binario automáticamente
- Crea un symlink en `~/.local/bin/` para acceso global
- Te avisa si necesitas añadir `~/.local/bin` a tu PATH

### Opción 2: Usando Makefile

Si prefieres usar make:

```bash
# Clonar el repositorio
git clone https://github.com/m4r10-tech/quanticshell.git
cd quanticshell

# Compilar e instalar
make install

# O solo compilar
make
```

### Opción 3: Binario precompilado (solo Linux x86_64)

Descarga el binario de la [última release](https://github.com/m4r10-tech/quanticshell/releases/latest):

```bash
# Descargar (cambia v2.0.0 por la última versión si es necesario)
curl -LO https://github.com/m4r10-tech/quanticshell/releases/latest/download/quanticshell-linux-x86_64
chmod +x quanticshell-linux-x86_64

# Ejecutar directamente
./quanticshell-linux-x86_64

# O instalarlo para acceso global
mkdir -p ~/.local/bin
cp quanticshell-linux-x86_64 ~/.local/bin/quanticshell
```

**Nota**: El binario precompilado solo funciona en Linux x86_64. Para otras arquitecturas o sistemas, usa la Opción 1 o 2.

### Opción 4: Compilación manual

Si prefieres compilar manualmente:

```bash
# Clonar el repositorio
git clone https://github.com/m4r10-tech/quanticshell.git
cd quanticshell

# Compilar
cc -std=c11 -Wall -Wextra -Wpedantic -O2 -o quanticshell quanticshell.c

# Ejecutar
./quanticshell
```

### Acceso global

Después de cualquier método de instalación, puedes ejecutar `quanticshell` desde cualquier directorio:

```bash
quanticshell
```

Si el comando no se encuentra, asegúrate de que `~/.local/bin` está en tu PATH:

```bash
# Añadir a ~/.bashrc o ~/.zshrc
export PATH="$HOME/.local/bin:$PATH"

# Recargar la configuración
source ~/.bashrc
```

## Uso

### Primer inicio

Al ejecutar la shell por primera vez, te pedirá configurar el prompt:

```
=== QUANTIC SHELL - CONFIGURACION INICIAL ===
Nombre (enter = usuario): mario
Host (enter = hostname): mi-pc
Simbolo (enter = $): >

Configuracion guardada en ~/.quanticshell/config
```

### Comandos internos

```bash
# Ver ayuda
help

# Cambiar de directorio
cd /ruta/al/directorio
cd          # Va al directorio home
cd ..       # Sube un nivel

# Configurar el prompt
config

# Gestionar aliases
alias                    # Lista todos los aliases
alias ll ls -la         # Crea un alias
alias gs git status     # Otro alias
alias gs='git status'   # También vale sintaxis bash con =
unalias ll              # Elimina un alias

# Builtins útiles
echo hola $USER $?      # Expansión de variables y último exit code
pwd
export MI_VAR=123; echo $MI_VAR
unset MI_VAR
history                 # Ver historial (history -c para limpiar)

# Jobs
sleep 30 &
jobs
fg %1
bg %1
wait

# Scripts / rc
source ~/.quanticshell/rc
```

### Aliases

Los aliases se guardan en `~/.quanticshell/aliases` con el formato:
```
nombre comando completo
```

Ejemplos de uso:
```bash
# Crear aliases útiles
alias ll ls -la
alias la ls -A
alias .. cd ..
alias gs git status
alias gp git push

# Usar los aliases
ll /tmp
gs
```

### Encadenamiento de comandos

Ejecuta múltiples comandos en una sola línea:

```bash
# Ejecutar comandos en secuencia
cd /tmp; pwd; ls

# Compilar y ejecutar en un paso
cc programa.c -o programa; ./programa

# Actualizar e instalar
sudo apt update; sudo apt upgrade
```

### Comandos externos (ejecutor propio, rápido)

Sin pasar por `/bin/sh` (solo se usa como fallback para `$(...)`, backticks, `&&`, `||`).
El estado de un pipeline es el del último comando (POSIX): `false | true` → 0.

```bash
# Pipes nativos
ls -la | grep ".c$" | wc -l

# Redirecciones nativas
echo "hola" > archivo.txt
cat archivo.txt >> otro.txt
cat < archivo.txt

# Globbing nativo
ls *.c
rm *.o

# Expansiones propias
echo $HOME ${USER} $? $$
cd ~/proyectos
```

### Historial y edición de línea

- **↑ (flecha arriba)**: Comando anterior
- **↓ (flecha abajo)**: Comando siguiente
- **← / →**: Moverse por la línea (también Ctrl+A inicio, Ctrl+E fin)
- **Tab**: Autocompleta comandos y ficheros (pulsa dos veces para listar)
- **Ctrl+R**: Búsqueda incremental en el historial
- **Ctrl+U / Ctrl+K / Ctrl+W**: Borrar línea / hasta el final / palabra
- **Supr**: Borrar bajo el cursor
- **Ctrl+C**: Cancela el comando actual o la línea en edición
- **Ctrl+Z**: Suspende el trabajo actual (luego `fg`)
- **Ctrl+D**: Sale de la shell (si la línea está vacía)

El historial guarda las últimas 500 líneas en `~/.quanticshell/history` (append, no reescribe el fichero).

### Arranque (`rc`)

Al iniciar se ejecuta `~/.quanticshell/rc` línea a línea (comentarios con `#`). Ejemplo:

```bash
# ~/.quanticshell/rc
alias ll ls -la
alias gs git status
export EDITOR=vim
```

## Estructura del proyecto

```
quanticshell/
├── quanticshell.c      # Código fuente completo de la shell (v2.0)
├── quanticshell        # Binario precompilado (Linux x86_64)
├── install.sh          # Script de instalación automática
├── Makefile            # Makefile para compilación manual
├── README.md           # Esta documentación
├── .gitignore          # Archivos ignorados por Git
└── LICENSE             # Licencia MIT

Archivos generados en runtime (en ~/.quanticshell/):
├── config              # Configuración del prompt
├── aliases             # Aliases persistentes
├── history             # Historial de comandos (append)
└── rc                  # Script de arranque (opcional)
```

## Arquitectura del código

El código está organizado en módulos funcionales dentro de un solo archivo:

### 1. Gestión de memoria y utilidades
- `copiar_texto()`: Copia segura de strings con snprintf
- `quitar_salto()`: Elimina saltos de línea
- `recortar()`: Elimina espacios en blanco

### 2. Persistencia
- `inicializar_rutas()`: Configura rutas de archivos en `~/.quanticshell/`
- `guardar_conf()` / `cargar_conf()`: Gestión de configuración
- `guardar_aliases()` / `cargar_aliases()`: Gestión de aliases
- `guardar_historial()` / `cargar_historial()`: Gestión de historial

### 3. Interfaz de usuario
- `mostrar_prompt()`: Muestra el prompt con colores y directorio actual
- `leer_linea_interactiva()`: Lee entrada con soporte para flechas
- `redibujar_linea()`: Redibuja la línea actual

### 4. Procesamiento de comandos (v2.0: ejecutor propio)
- `ejecutar_linea()`: Parsea `;` respetando comillas
- `expandir_alias_linea()`: Expansión de aliases (hasta 8 niveles)
- `parsear_tramo()`: Tokeniza con comillas + expande `$VAR`/`${VAR}`/`$?`/`$$`/`~` + glob
- `ejecutar_pipeline()`: Pipes nativos con `fork`/`execvp` (vía rápida sin `/bin/sh`)
- `ejecutar_con_sh()`: Fallback a `/bin/sh -c` solo para `$(...)`, backticks, `&&`, `||`
- Builtins en el padre (con redirecciones) o en hijo si van en pipe/background

### 5. Jobs
- `agregar_job()` / `limpiar_jobs()` / `buscar_job()`
- `esperar_foreground()`: `tcsetpgrp` + `waitpid` con `WUNTRACED`
- `cmd_jobs` / `cmd_fg` / `cmd_bg` / `cmd_wait`

### 6. Línea interactiva
- `leer_linea_interactiva()`: Inserción a mitad de línea, ←/→, Ctrl+A/E/U/K/W, Supr, Ctrl+Z
- `completar_tab()` / `recoger_candidatos()`: Completion de builtins, aliases, PATH y ficheros
- `busqueda_reversa()`: Ctrl+R incremental

### 7. Prompt rápido
- `obtener_rama_git()`: Lee `.git/HEAD` sin hacer fork
- `mostrar_prompt()`: Muestra `[exit-code]` en rojo + rama en amarillo

### 8. Manejo de señales
- `manejar_sigint()`: Handler para Ctrl+C
- `restaurar_terminal()`: Restaura configuración del terminal

## Conceptos de C que practica

### Fundamentos
- Arrays de caracteres y terminación con `\0`
- Punteros y aritmética de punteros
- Estructuras (`struct`) y arrays de estructuras
- Funciones y paso de parámetros

### Strings y memoria
- `strcmp`, `strncmp`: Comparación de strings
- `strlen`: Longitud de strings
- `snprintf`: Formato seguro
- `memmove`: Movimiento de memoria con overlap
- `strcspn`: Búsqueda de caracteres

### Entrada/Salida
- `fgets`: Lectura de líneas
- `printf`, `fprintf`: Salida formateada
- `fopen`, `fclose`: Gestión de archivos
- `read`: Lectura de bajo nivel

### Sistema de archivos
- `getcwd`: Directorio actual
- `chdir`: Cambiar directorio
- `mkdir`: Crear directorios
- `stat`: Información de archivos

### Procesos POSIX
- `fork`: Crear procesos hijos
- `execl`: Ejecutar programas
- `waitpid`: Esperar procesos hijos
- `_exit`: Terminar proceso

### Señales
- `sigaction`: Instalar handlers
- `SIGINT`: Manejo de Ctrl+C
- `sig_atomic_t`: Variables atómicas para señales

### Terminal
- `termios`: Configuración del terminal
- `tcgetattr`, `tcsetattr`: Leer/escribir configuración
- `isatty`: Detectar si es terminal
- `read`: Lectura carácter por carácter

## Limitaciones actuales (v2.0)

- **Sin `&&` / `||` nativos**: se delegan a `/bin/sh` (compatibilidad, no velocidad)
- **Sin sustitución de comandos nativa**: `$(...)` y backticks van a `/bin/sh`
- **Expansiones `${...}` complejas** (`${V:-def}`, `${#V}`): van a `/bin/sh`; `$V` y `${V}` simples son nativas
- **Sin scripts con control de flujo**: `source`/`rc` ejecutan línea a línea (sin `if`/`for`, anti-recursión a 16 niveles)
- **Sin job control avanzado de terminal**: no hay `disown`

## Cómo extender el proyecto

Añadir un builtin son 3 pasos en `quanticshell.c`:

```c
// 1. Función (devuelve 1, ajusta ultimo_estado: 0 = ok)
static int cmd_mio(char **argv) {
    printf("hola %s\n", argv[1] ? argv[1] : "mundo");
    ultimo_estado = 0;
    return 1;
}
// 2. Declararla con el resto de cmd_* y registrarla en comandos[]:
//    {"mio", cmd_mio},
// 3. Documentarla en cmd_help y en este README (sección Uso).
```

Puntos de extensión:

- Nueva sintaxis → `dividir_puntoycoma()` / `dividir_pipes()` / `extraer_background()` + lista de fallback en `necesita_sh()`
- Nueva expansión → solo `expandir_token()`
- Nuevo estado global → sección "Config y estado global", con su `MAX_*`

Flags útiles para scripts y tests: `quanticshell -c 'comando'`, `--version`, `--help`.

## Pruebas

Suite automatizada (39 casos: compilación limpia, builtins, variables, pipes, redirecciones, glob, exit codes, jobs, aliases, `source`, salida no interactiva). Corre con `HOME` aislado en un temporal: no toca tu `~/.quanticshell` real.

```bash
make test
```

Chequeo manual rápido (también cubierto por la suite):

```bash
cc -std=c11 -Wall -Wextra -Wpedantic -O2 -o quanticshell quanticshell.c
./quanticshell --version
./quanticshell -c 'echo a; echo b | tr a-z A-Z'
./quanticshell -c 'echo x > /tmp/q.txt; cat < /tmp/q.txt; echo y >> /tmp/q.txt; cat /tmp/q.txt'
./quanticshell -c 'export A=5; echo ${A} $?; unset A'
./quanticshell -c 'echo *.c; echo "dq $HOME"; echo '"'"'sq $HOME'"'"
./quanticshell -c 'false; echo code:$?'
./quanticshell -c 'sleep 0.05 &; jobs; wait; jobs; echo done'
./quanticshell -c 'echo $(echo sub); echo ${USER:-anon}'
```

## Rendimiento

Optimizaciones de velocidad (sin añadir funciones):

- `-O2` en Makefile/install.sh: binario ~14% más pequeño (65KB → 55KB).
- Prompt precalculado una vez por comando; el repintado por tecla reutiliza el buffer (antes: `getcwd` + hasta 6 `fopen` de `.git/HEAD` por pulsación → ahora 0 syscalls).
- Rama git cacheada por `(cwd, mtime de HEAD)`: caso común = 1 `stat`, sin `fork`.
- Cursor atrás en un solo escape `\x1b[nD` (antes: un write por columna).
- Historial en append, el modo no interactivo no toca el historial.

Medido en Debian x86_64, 200 iteraciones (incluye ruido de `fork`+`exec` del propio benchmark):

- Arranque `quanticshell -c exit`: ~2,1 ms.
- 40 builtins por invocación: ~2,4 ms (~6 µs por `echo` extra).

## Roadmap

- [x] Ejecutor propio rápido (`execvp` + pipes + redirecciones + glob)
- [x] Autocompletado con Tab + edición de línea + Ctrl+R
- [x] Builtins + variables + `~/.quanticshell/rc`
- [x] Job control (`&`, `jobs`, `fg`, `bg`, `wait`, Ctrl+Z)
- [x] Prompt con git + exit code + historial por append
- [ ] Operadores `&&` / `||` nativos
- [ ] Sustitución de comandos `$()` nativa
- [ ] Variables de shell internas (no solo entorno)
- [ ] Colores configurables / temas de prompt
- [ ] Tests automatizados

## Contribuciones

Las contribuciones son bienvenidas. Si quieres contribuir:

1. Haz un fork del repositorio
2. Crea una rama para tu feature (`git checkout -b feature/AmazingFeature`)
3. Commit tus cambios (`git commit -m 'Add some AmazingFeature'`)
4. Push a la rama (`git push origin feature/AmazingFeature`)
5. Abre un Pull Request

### Estilo de código

- Usa C11 estándar
- Compila sin warnings con `-Wall -Wextra -Wpedantic -O2`
- Mantén el código simple y legible
- Añade comentarios para código complejo
- Sigue las convenciones existentes

## Licencia

Este proyecto está bajo la Licencia MIT - ver el archivo [LICENSE](LICENSE) para más detalles.

## Agradecimientos

- Inspirado por shells como bash, zsh y fish
- Diseñado como herramienta educativa para aprender C y POSIX
- Gracias a la comunidad de código abierto

## Contacto

Para preguntas o sugerencias, abre un issue en GitHub.

---

**Nota**: v2.0 ya es usable en el día a día (pipes, redirecciones, jobs, completion, git prompt). Para sistemas críticos se recomienda igualmente bash/zsh.
