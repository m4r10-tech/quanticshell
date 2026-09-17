#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* =====================================================================
 * Quantic Shell - shell interactiva en C (POSIX).
 *
 * Mapa del archivo (un solo fichero a propósito: proyecto educativo):
 *   1. Config y estado global      (límites, prompt, historial, jobs)
 *   2. Utilidades                  (strings seguros con snprintf)
 *   3. Señales y terminal          (SIGINT, job-control, modo raw)
 *   4. Persistencia                (~/.quanticshell/: config, aliases,
 *                                   history en append, rc de arranque)
 *   5. Prompt                      (git sin fork + último exit code)
 *   6. Edición de línea            (raw mode, historial, Tab, Ctrl+R)
 *   7. Builtins                    (tabla `comandos`: ver cómo ampliar abajo)
 *   8. Parser + expansión          (;, pipes, redirecciones, $VAR, glob)
 *   9. Ejecutor + jobs             (fork/execvp, fallback /bin/sh, fg/bg)
 *  10. main                        (flags -c/--version, bucle REPL)
 *
 * CÓMO AÑADIR UN BUILTIN (3 pasos):
 *   1. Escribe `static int cmd_mio(char **argv)` que devuelva 1 y ajuste
 *      `ultimo_estado` (0 = ok). argv[0] es el nombre, argv termina en NULL.
 *   2. Decláralo junto al resto de `cmd_*` y regístralo en la tabla
 *      `comandos[]` con {"mio", cmd_mio}.
 *   3. Documenta su uso en `cmd_help` y en el README (sección Uso).
 *   Los builtins corren en el padre si van solos (pueden hacer cd/export),
 *   y en un hijo si van en pipe o background.
 *
 * CÓMO AMPLIAR LA SINTAXIS:
 *   - Nuevo operador: extiende `dividir_*`/`extraer_background` y el
 *     fallback `necesita_sh()` (lista lo que aún delega a /bin/sh).
 *   - Nueva expansión: toca solo `expandir_token()`.
 * ===================================================================== */

#define QUANTIC_VERSION "2.0.0"

#define MAX_ALIAS 64
#define MAX_LINEA 4096
#define MAX_HISTORIAL 500
#define MAX_RUTA 1024
#define MAX_ARGS 128
#define MAX_JOBS 32
#define MAX_TRAMOS_PIPE 16
#define MAX_SOURCE_DEPTH 16

#define NOMBRE_CONF "config"
#define NOMBRE_ALIASES "aliases"
#define NOMBRE_HISTORIAL "history"
#define NOMBRE_RC "rc"

typedef struct {
    char nombre[32];
    char comando[512];
} Alias;

typedef struct {
    const char *nombre;
    int (*funcion)(char **argv);
} Comando;

typedef struct {
    int id;
    pid_t pgid;
    char cmd[MAX_LINEA];
    int detenido;
    int activo;
} Job;

static Alias aliases[MAX_ALIAS];
static int num_aliases = 0;

static char historial[MAX_HISTORIAL][MAX_LINEA];
static int num_historial = 0;

static char prompt_nombre[32] = "usuario";
static char prompt_host[32] = "localhost";
static char prompt_simbolo[4] = "$";

static char ruta_conf[MAX_RUTA];
static char ruta_aliases[MAX_RUTA];
static char ruta_historial[MAX_RUTA];
static char ruta_rc[MAX_RUTA];
static int persistencia_disponible = 0;

static volatile sig_atomic_t sigint_recibido = 0;
static struct termios terminal_original;
static int terminal_activo = 0;

static int ultimo_estado = 0;
static int usar_color = 1; /* 0 si stdout no es tty o existe NO_COLOR */
static Job trabajos[MAX_JOBS];
static int siguiente_job_id = 1;
static pid_t shell_pgid = 0;

/* ---------- utilidades ---------- */

/* Copia segura con truncado: nunca desborda (semántica idéntica a snprintf
   pero sin warnings de -Wformat-truncation a -O2, donde el inline expone
   los tamaños). */
static void copiar_texto(char *destino, size_t tam_destino, const char *origen) {
    if (tam_destino == 0) return;
    if (!origen) { destino[0] = '\0'; return; }
    size_t n = strlen(origen);
    if (n >= tam_destino) n = tam_destino - 1;
    memcpy(destino, origen, n);
    destino[n] = '\0';
}

static void quitar_salto(char *texto) {
    texto[strcspn(texto, "\n")] = '\0';
}

static char *recortar(char *texto) {
    while (*texto && isspace((unsigned char)*texto)) texto++;
    char *fin = texto + strlen(texto);
    while (fin > texto && isspace((unsigned char)fin[-1])) {
        *--fin = '\0';
    }
    return texto;
}

/* ---------- señales y terminal ---------- */

static void manejar_sigint(int signo) {
    (void)signo;
    sigint_recibido = 1;
}

static int cancelar_entrada(void) {
    sigint_recibido = 0;
    printf("\n");
    fflush(stdout);
    return -1;
}

static void restaurar_terminal(void) {
    if (terminal_activo) {
        tcsetattr(STDIN_FILENO, TCSANOW, &terminal_original);
        terminal_activo = 0;
    }
}

static void instalar_manejadores(void) {
    struct sigaction accion;
    memset(&accion, 0, sizeof(accion));
    accion.sa_handler = manejar_sigint;
    sigemptyset(&accion.sa_mask);
    accion.sa_flags = SA_RESTART;
    sigaction(SIGINT, &accion, NULL);

    /* La shell no debe pararse por estas señales; los hijos sí las reciben */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
}

static void restaurar_senales_para_hijo(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
}

/* ---------- persistencia ---------- */

static void inicializar_rutas(void) {
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        fprintf(stderr, "Aviso: HOME no está definido; no habrá persistencia.\n");
        return;
    }
    char directorio_datos[MAX_RUTA];
    int escritos = snprintf(directorio_datos, sizeof(directorio_datos), "%s/.quanticshell", home);
    if (escritos < 0 || (size_t)escritos >= sizeof(directorio_datos)) {
        fprintf(stderr, "Aviso: la ruta de datos es demasiado larga; no habrá persistencia.\n");
        return;
    }
    if (mkdir(directorio_datos, 0700) == -1 && errno != EEXIST) {
        perror("No se pudo crear ~/.quanticshell");
        return;
    }
    struct stat info;
    if (stat(directorio_datos, &info) == -1 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "Aviso: ~/.quanticshell no es un directorio; no habrá persistencia.\n");
        return;
    }
    int c = snprintf(ruta_conf, sizeof(ruta_conf), "%s/%s", directorio_datos, NOMBRE_CONF);
    int a = snprintf(ruta_aliases, sizeof(ruta_aliases), "%s/%s", directorio_datos, NOMBRE_ALIASES);
    int h = snprintf(ruta_historial, sizeof(ruta_historial), "%s/%s", directorio_datos, NOMBRE_HISTORIAL);
    int r = snprintf(ruta_rc, sizeof(ruta_rc), "%s/%s", directorio_datos, NOMBRE_RC);
    if (c < 0 || a < 0 || h < 0 || r < 0 ||
        (size_t)c >= sizeof(ruta_conf) || (size_t)a >= sizeof(ruta_aliases) ||
        (size_t)h >= sizeof(ruta_historial) || (size_t)r >= sizeof(ruta_rc)) {
        fprintf(stderr, "Aviso: una ruta de datos es demasiado larga; no habrá persistencia.\n");
        return;
    }
    persistencia_disponible = 1;
}

static void guardar_conf(void) {
    if (!persistencia_disponible) return;
    FILE *f = fopen(ruta_conf, "w");
    if (!f) { perror("No se pudo guardar la configuración"); return; }
    fprintf(f, "nombre=%s\n", prompt_nombre);
    fprintf(f, "host=%s\n", prompt_host);
    fprintf(f, "simbolo=%s\n", prompt_simbolo);
    if (fclose(f) == EOF) perror("No se pudo cerrar la configuración");
}

static int cargar_conf(void) {
    if (!persistencia_disponible) return 0;
    FILE *f = fopen(ruta_conf, "r");
    if (!f) return 0;
    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        quitar_salto(linea);
        if (strncmp(linea, "nombre=", 7) == 0) copiar_texto(prompt_nombre, sizeof(prompt_nombre), linea + 7);
        else if (strncmp(linea, "host=", 5) == 0) copiar_texto(prompt_host, sizeof(prompt_host), linea + 5);
        else if (strncmp(linea, "simbolo=", 8) == 0) copiar_texto(prompt_simbolo, sizeof(prompt_simbolo), linea + 8);
    }
    fclose(f);
    return 1;
}

static void guardar_aliases(void) {
    if (!persistencia_disponible) return;
    FILE *f = fopen(ruta_aliases, "w");
    if (!f) { perror("No se pudieron guardar los aliases"); return; }
    for (int i = 0; i < num_aliases; i++) {
        fprintf(f, "%s %s\n", aliases[i].nombre, aliases[i].comando);
    }
    if (fclose(f) == EOF) perror("No se pudieron cerrar los aliases");
}

static void cargar_aliases(void) {
    if (!persistencia_disponible) return;
    FILE *f = fopen(ruta_aliases, "r");
    if (!f) return;
    char linea[1024];
    while (fgets(linea, sizeof(linea), f) && num_aliases < MAX_ALIAS) {
        char nombre[32];
        char comando[512];
        quitar_salto(linea);
        if (sscanf(linea, "%31s %511[^\n]", nombre, comando) == 2) {
            copiar_texto(aliases[num_aliases].nombre, sizeof(aliases[num_aliases].nombre), nombre);
            copiar_texto(aliases[num_aliases].comando, sizeof(aliases[num_aliases].comando), comando);
            num_aliases++;
        }
    }
    fclose(f);
}

static int buscar_alias(const char *nombre) {
    for (int i = 0; i < num_aliases; i++) {
        if (strcmp(aliases[i].nombre, nombre) == 0) return i;
    }
    return -1;
}

/* Historial: carga completa al inicio, append por línea (rápido) */
static void agregar_historial_memoria(const char *linea) {
    if (linea[0] == '\0') return;
    if (num_historial > 0 && strcmp(historial[num_historial - 1], linea) == 0) return;
    if (num_historial == MAX_HISTORIAL) {
        memmove(historial, historial + 1, sizeof(historial[0]) * (MAX_HISTORIAL - 1));
        num_historial--;
    }
    copiar_texto(historial[num_historial], sizeof(historial[num_historial]), linea);
    num_historial++;
}

static void anexar_historial_archivo(const char *linea) {
    if (!persistencia_disponible || linea[0] == '\0') return;
    FILE *f = fopen(ruta_historial, "a");
    if (!f) return;
    fprintf(f, "%s\n", linea);
    fclose(f);
}

static void agregar_historial(const char *linea) {
    size_t n = num_historial;
    agregar_historial_memoria(linea);
    if ((size_t)num_historial != n) anexar_historial_archivo(linea);
}

static void cargar_historial(void) {
    if (!persistencia_disponible) return;
    FILE *f = fopen(ruta_historial, "r");
    if (!f) return;
    char linea[MAX_LINEA];
    while (fgets(linea, sizeof(linea), f)) {
        quitar_salto(linea);
        agregar_historial_memoria(linea);
    }
    fclose(f);
}

/* ---------- prompt con git + exit code (sin fork, rápido) ---------- */

/* Parsea un .git/HEAD ya localizado. Devuelve 1 con la rama en dest. */
static int leer_head_para_rama(const char *head_path, char *dest, size_t tam) {
    FILE *f = fopen(head_path, "r");
    if (!f) return 0;
    char head[512];
    int ok = 0;
    if (fgets(head, sizeof(head), f)) {
        quitar_salto(head);
        if (strncmp(head, "ref: refs/heads/", 16) == 0) {
            copiar_texto(dest, tam, head + 16);
            ok = 1;
        } else if (strncmp(head, "ref: ", 5) == 0) {
            const char *slash = strrchr(head, '/');
            copiar_texto(dest, tam, slash ? slash + 1 : head + 5);
            ok = 1;
        } else if (strlen(head) >= 7) {
            /* detached HEAD: hash corto */
            char corto[16];
            snprintf(corto, sizeof(corto), "%.7s", head);
            copiar_texto(dest, tam, corto);
            ok = 1;
        }
    }
    fclose(f);
    return ok;
}

/* Caché de rama git: mientras no cambie el cwd ni el mtime de .git/HEAD,
   el coste es 1 stat (antes: hasta 6 fopen + getcwd por cada pulsación). */
static char git_cwd_cache[MAX_RUTA] = "";
static char git_head_path[MAX_RUTA] = "";
static time_t git_head_mtime = 0;
static char git_rama_cache[128] = "";
static int git_cache_tiene = 0;
static int git_cache_valida = 0;

static int obtener_rama_git_cache(const char *cwd, char *dest, size_t tam) {
    if (git_cache_valida && strcmp(cwd, git_cwd_cache) == 0) {
        if (git_head_path[0] == '\0') return 0; /* negativo cacheado */
        struct stat st;
        if (stat(git_head_path, &st) == 0 && st.st_mtime == git_head_mtime) {
            if (!git_cache_tiene) return 0;
            copiar_texto(dest, tam, git_rama_cache);
            return 1;
        }
    }
    git_cache_valida = 0;
    char ruta[MAX_RUTA];
    /* subir hasta 6 niveles buscando .git/HEAD */
    for (int nivel = 0; nivel < 6; nivel++) {
        int n;
        if (nivel == 0) n = snprintf(ruta, sizeof(ruta), "%s/.git/HEAD", cwd);
        else {
            char tmp[MAX_RUTA];
            copiar_texto(tmp, sizeof(tmp), cwd);
            for (int i = 0; i < nivel; i++) {
                char *barra = strrchr(tmp, '/');
                if (!barra || barra == tmp) { tmp[1] = '\0'; if (barra == tmp) tmp[1] = '\0'; break; }
                *barra = '\0';
            }
            n = snprintf(ruta, sizeof(ruta), "%s/.git/HEAD", tmp);
        }
        if (n < 0 || (size_t)n >= sizeof(ruta)) return 0;
        struct stat st;
        if (stat(ruta, &st) != 0) {
            /* si cwd es "/" ya no hay más que subir */
            if (strcmp(cwd, "/") == 0) break;
            continue;
        }
        /* HEAD encontrado: parsear y cachear (cwd + mtime como clave). */
        copiar_texto(git_cwd_cache, sizeof(git_cwd_cache), cwd);
        copiar_texto(git_head_path, sizeof(git_head_path), ruta);
        git_head_mtime = st.st_mtime;
        git_cache_tiene = leer_head_para_rama(ruta, git_rama_cache, sizeof(git_rama_cache));
        git_cache_valida = 1;
        if (!git_cache_tiene) return 0;
        copiar_texto(dest, tam, git_rama_cache);
        return 1;
    }
    /* Sin repo: cachear el negativo hasta cambiar de directorio. */
    copiar_texto(git_cwd_cache, sizeof(git_cwd_cache), cwd);
    git_head_path[0] = '\0';
    git_cache_tiene = 0;
    git_cache_valida = 1;
    return 0;
}

/* Prompt precalculado una vez por comando. El repintado por tecla
   (redibujar_linea_pos) reutiliza `prompt_actual` sin syscalls: antes hacía
   getcwd + hasta 6 fopen por cada pulsación. Solo se reconstruye al mostrar
   un prompt nuevo (el cwd solo puede cambiar vía `cd`). */
static char prompt_actual[2048] = "";

static void actualizar_prompt(void) {
    char cwd[MAX_RUTA];
    if (!getcwd(cwd, sizeof(cwd))) copiar_texto(cwd, sizeof(cwd), "?");
    char ruta[MAX_RUTA];
    copiar_texto(ruta, sizeof(ruta), cwd);
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        size_t home_len = strlen(home);
        if (strncmp(home, ruta, home_len) == 0 &&
            (ruta[home_len] == '/' || ruta[home_len] == '\0')) {
            memmove(ruta + 1, ruta + home_len, strlen(ruta + home_len) + 1);
            ruta[0] = '~';
        }
    }
    char rama[128] = "";
    int tiene_git = obtener_rama_git_cache(cwd, rama, sizeof(rama));

    /* Un solo snprintf por piezas en el buffer (menos writes que N printf).
       Sin color (pipe/NO_COLOR): prompt plano, apto para scripts. */
    size_t usado = 0;
    size_t resto = sizeof(prompt_actual);
    int n = 0;
    if (ultimo_estado != 0) {
        n = usar_color ? snprintf(prompt_actual + usado, resto, "\x1b[31m[%d]\x1b[0m ", ultimo_estado)
                       : snprintf(prompt_actual + usado, resto, "[%d] ", ultimo_estado);
        if (n > 0 && (size_t)n < resto) { usado += (size_t)n; resto -= (size_t)n; }
    }
    n = usar_color ? snprintf(prompt_actual + usado, resto, "\x1b[32m%s\x1b[0m@\x1b[34m%s\x1b[0m \x1b[35m%s\x1b[0m",
                              prompt_nombre, prompt_host, ruta)
                   : snprintf(prompt_actual + usado, resto, "%s@%s %s",
                              prompt_nombre, prompt_host, ruta);
    if (n > 0 && (size_t)n < resto) { usado += (size_t)n; resto -= (size_t)n; }
    if (tiene_git) {
        n = usar_color ? snprintf(prompt_actual + usado, resto, " \x1b[33m(%s)\x1b[0m", rama)
                       : snprintf(prompt_actual + usado, resto, " (%s)", rama);
        if (n > 0 && (size_t)n < resto) { usado += (size_t)n; resto -= (size_t)n; }
    }
    n = usar_color ? snprintf(prompt_actual + usado, resto, " \x1b[36m%s\x1b[0m ", prompt_simbolo)
                   : snprintf(prompt_actual + usado, resto, " %s ", prompt_simbolo);
    (void)n;
}

static void mostrar_prompt(void) {
    actualizar_prompt();
    fputs(prompt_actual, stdout);
    fflush(stdout);
}

/* ---------- lectura interactiva avanzada ---------- */

static int activar_modo_edicion(void) {
    if (tcgetattr(STDIN_FILENO, &terminal_original) == -1) return 0;
    struct termios modo = terminal_original;
    modo.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    modo.c_cc[VMIN] = 1;
    modo.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &modo) == -1) return 0;
    terminal_activo = 1;
    return 1;
}

static int leer_caracter_terminal(char *caracter) {
    ssize_t leidos = read(STDIN_FILENO, caracter, 1);
    if (leidos == 1) return 1;
    if (leidos == 0) return 0;
    if (errno == EINTR) return -1;
    return -2;
}

/* Autosugerencia estilo fish: texto fantasma del historial (dim) que se
   acepta con →. Solo display: nunca entra al buffer sin aceptar.
   Coste por tecla: <= MAX_HISTORIAL prefix-cmps (µs). Solo con cursor al final. */
static char sugerencia_actual[MAX_LINEA] = "";

static void buscar_sugerencia(const char *linea) {
    sugerencia_actual[0] = '\0';
    size_t len = strlen(linea);
    if (len == 0) return;
    for (int i = num_historial - 1; i >= 0; i--) {
        if (strncmp(historial[i], linea, len) == 0 && historial[i][len] != '\0') {
            copiar_texto(sugerencia_actual, sizeof(sugerencia_actual), historial[i] + len);
            return;
        }
    }
}

static void redibujar_linea_pos(const char *linea, size_t pos) {
    size_t len = strlen(linea);
    size_t suflen = 0;
    if (pos == len) {
        buscar_sugerencia(linea);
        suflen = strlen(sugerencia_actual);
    } else {
        sugerencia_actual[0] = '\0';
    }
    printf("\r\x1b[2K");
    fputs(prompt_actual, stdout); /* caché: 0 syscalls (ver actualizar_prompt) */
    fputs(linea, stdout);
    if (suflen > 0) printf("\x1b[2m%s\x1b[0m", sugerencia_actual);
    /* mover cursor atrás en un solo escape (antes: un write por columna) */
    if (len + suflen > pos) printf("\x1b[%zuD", len + suflen - pos);
    fflush(stdout);
}

/* --- tab completion --- */
static int es_ejecutable_en_path(const char *dir, const char *nombre) {
    char ruta[MAX_RUTA];
    int n = snprintf(ruta, sizeof(ruta), "%s/%s", dir, nombre);
    if (n < 0 || (size_t)n >= sizeof(ruta)) return 0;
    if (access(ruta, X_OK) != 0) return 0;
    struct stat st;
    if (stat(ruta, &st) == -1) return 0;
    return S_ISREG(st.st_mode);
}

/* declaración adelantada para completar builtins */
static int cmd_exit(char **argv);
static int cmd_cd(char **argv);
static int cmd_help(char **argv);
static int cmd_alias(char **argv);
static int cmd_unalias(char **argv);
static int cmd_config(char **argv);
static int cmd_echo(char **argv);
static int cmd_pwd(char **argv);
static int cmd_export(char **argv);
static int cmd_unset(char **argv);
static int cmd_true(char **argv);
static int cmd_false(char **argv);
static int cmd_noop(char **argv);
static int cmd_test(char **argv);
static int cmd_history(char **argv);
static int cmd_jobs(char **argv);
static int cmd_fg(char **argv);
static int cmd_bg(char **argv);
static int cmd_wait(char **argv);
static int cmd_source(char **argv);

static Comando comandos[] = {
    {"exit", cmd_exit},
    {"cd", cmd_cd},
    {"help", cmd_help},
    {"alias", cmd_alias},
    {"unalias", cmd_unalias},
    {"config", cmd_config},
    {"echo", cmd_echo},
    {"pwd", cmd_pwd},
    {"export", cmd_export},
    {"unset", cmd_unset},
    {"true", cmd_true},
    {"false", cmd_false},
    {":", cmd_noop},
    {"test", cmd_test},
    {"[", cmd_test},
    {"history", cmd_history},
    {"jobs", cmd_jobs},
    {"fg", cmd_fg},
    {"bg", cmd_bg},
    {"wait", cmd_wait},
    {"source", cmd_source},
    {NULL, NULL}
};

static void recoger_candidatos(const char *prefijo, int primera_palabra,
                               char candidatos[][256], int *n_cand, int max_cand) {
    *n_cand = 0;
    size_t pre_len = strlen(prefijo);
    /* separar directorio base para ficheros */
    const char *barra = strrchr(prefijo, '/');
    const char *dir = ".";
    const char *base = prefijo;
    char dirbuf[MAX_RUTA] = ".";
    if (barra) {
        size_t dl = (size_t)(barra - prefijo);
        if (dl == 0) copiar_texto(dirbuf, sizeof(dirbuf), "/");
        else { snprintf(dirbuf, sizeof(dirbuf), "%.*s", (int)dl, prefijo); }
        dir = dirbuf;
        base = barra + 1;
    }
    size_t base_len = strlen(base);

    if (primera_palabra && !barra) {
        /* builtins */
        for (int i = 0; comandos[i].nombre; i++) {
            if (strncmp(comandos[i].nombre, prefijo, pre_len) == 0 && *n_cand < max_cand) {
                copiar_texto(candidatos[*n_cand], sizeof(candidatos[0]), comandos[i].nombre);
                (*n_cand)++;
            }
        }
        /* aliases */
        for (int i = 0; i < num_aliases && *n_cand < max_cand; i++) {
            if (strncmp(aliases[i].nombre, prefijo, pre_len) == 0) {
                copiar_texto(candidatos[*n_cand], sizeof(candidatos[0]), aliases[i].nombre);
                (*n_cand)++;
            }
        }
        /* binarios en PATH */
        const char *path = getenv("PATH");
        if (path) {
            char *copia = malloc(strlen(path) + 1);
            if (copia) {
                strcpy(copia, path);
                for (char *d = strtok(copia, ":"); d && *n_cand < max_cand + 100; d = strtok(NULL, ":")) {
                    DIR *dp = opendir(d);
                    if (!dp) continue;
                    struct dirent *e;
                    while ((e = readdir(dp)) && *n_cand < max_cand + 100) {
                        if (strncmp(e->d_name, prefijo, pre_len) != 0) continue;
                        /* evitar duplicados */
                        int dup = 0;
                        for (int k = 0; k < *n_cand; k++) {
                            if (strcmp(candidatos[k], e->d_name) == 0) { dup = 1; break; }
                        }
                        if (dup) continue;
                        if (!es_ejecutable_en_path(d, e->d_name)) continue;
                        if (*n_cand < max_cand) {
                            snprintf(candidatos[*n_cand], 256, "%s", e->d_name);
                            (*n_cand)++;
                        }
                    }
                    closedir(dp);
                }
                free(copia);
            }
        }
    }
    /* ficheros (siempre, también para primera palabra con '/') */
    {
        DIR *dp = opendir(dir);
        if (dp) {
            struct dirent *e;
            while ((e = readdir(dp)) && *n_cand < max_cand) {
                if (e->d_name[0] == '.' && base[0] != '.') continue;
                if (strncmp(e->d_name, base, base_len) != 0) continue;
                int dup = 0;
                for (int k = 0; k < *n_cand; k++) {
                    const char *cb = strrchr(candidatos[k], '/');
                    const char *cn = cb ? cb + 1 : candidatos[k];
                    if (strcmp(cn, e->d_name) == 0) { dup = 1; break; }
                }
                if (dup) continue;
                /* Construcción acotada (evita truncados silenciosos): si el
                   candidato no cabe, se omite; no se puede completar igual. */
                if (barra) {
                    size_t dl = (size_t)(barra - prefijo);
                    size_t nl = strlen(e->d_name);
                    if (dl + 1 + nl + 1 > sizeof(candidatos[0])) continue;
                    memcpy(candidatos[*n_cand], prefijo, dl);
                    candidatos[*n_cand][dl] = '/';
                    memcpy(candidatos[*n_cand] + dl + 1, e->d_name, nl + 1);
                } else {
                    size_t nl = strlen(e->d_name);
                    if (nl + 1 > sizeof(candidatos[0])) continue;
                    memcpy(candidatos[*n_cand], e->d_name, nl + 1);
                }
                /* añadir '/' si es directorio */
                char rt[MAX_RUTA];
                {
                    size_t dl = strlen(dir);
                    size_t nl = strlen(e->d_name);
                    if (dl + 1 + nl + 1 <= sizeof(rt)) {
                        memcpy(rt, dir, dl);
                        rt[dl] = '/';
                        memcpy(rt + dl + 1, e->d_name, nl + 1);
                    } else {
                        rt[0] = '\0';
                    }
                }
                struct stat st;
                if (rt[0] && stat(rt, &st) == 0 && S_ISDIR(st.st_mode)) {
                    size_t l = strlen(candidatos[*n_cand]);
                    if (l + 1 < 256) { candidatos[*n_cand][l] = '/'; candidatos[*n_cand][l+1] = '\0'; }
                }
                (*n_cand)++;
            }
            closedir(dp);
        }
    }
}

/* completa la palabra bajo el cursor; devuelve 1 si cambió */
static int completar_tab(char *linea, size_t *len, size_t *pos) {
    size_t inicio = *pos;
    while (inicio > 0 && !isspace((unsigned char)linea[inicio - 1])) inicio--;
    char prefijo[512];
    size_t pre_len = *pos - inicio;
    if (pre_len >= sizeof(prefijo)) return 0;
    memcpy(prefijo, linea + inicio, pre_len);
    prefijo[pre_len] = '\0';

    /* ¿primera palabra? */
    int primera = 1;
    for (size_t i = 0; i < inicio; i++) {
        if (!isspace((unsigned char)linea[i])) { /* hay algo antes, ver si solo espacios */
            primera = 0;
            break;
        }
    }
    /* más preciso: si hay no-espacio antes del inicio salvo espacios -> no primera */
    {
        int solo_espacios = 1;
        for (size_t i = 0; i < inicio; i++) {
            if (!isspace((unsigned char)linea[i])) solo_espacios = 0;
        }
        primera = solo_espacios;
    }

    char candidatos[120][256];
    int n_cand = 0;
    recoger_candidatos(prefijo, primera, candidatos, &n_cand, 120);

    if (n_cand == 0) return 0;
    if (n_cand == 1) {
        size_t cl = strlen(candidatos[0]);
        size_t resto = cl - pre_len;
        if (*len + resto + 1 >= MAX_LINEA) return 0;
        memmove(linea + inicio + cl, linea + *pos, *len - *pos + 1);
        memcpy(linea + inicio, candidatos[0], cl);
        /* si es una única coincidencia y no termina en '/', añadir espacio */
        if (candidatos[0][cl - 1] != '/') {
            memmove(linea + inicio + cl + 1, linea + inicio + cl, strlen(linea + inicio + cl) + 1);
            linea[inicio + cl] = ' ';
            *len += cl - pre_len + 1;
            *pos = inicio + cl + 1;
        } else {
            *len += resto;
            *pos = inicio + cl;
        }
        redibujar_linea_pos(linea, *pos);
        return 1;
    }
    /* prefijo común */
    size_t comun = strlen(candidatos[0]);
    for (int i = 1; i < n_cand; i++) {
        size_t j = 0;
        while (j < comun && candidatos[0][j] && candidatos[0][j] == candidatos[i][j]) j++;
        comun = j;
    }
    /* comparar solo la parte tras el prefijo escrito */
    const char *barra = strrchr(prefijo, '/');
    size_t base_off = barra ? (size_t)(barra + 1 - prefijo) : 0;
    (void)base_off;
    if (comun > pre_len) {
        size_t extra = comun - pre_len;
        if (*len + extra >= MAX_LINEA) return 0;
        memmove(linea + *pos + extra, linea + *pos, *len - *pos + 1);
        memcpy(linea + *pos, candidatos[0] + pre_len, extra);
        *len += extra;
        *pos += extra;
        redibujar_linea_pos(linea, *pos);
        return 1;
    }
    /* listar */
    printf("\n");
    for (int i = 0; i < n_cand; i++) printf("%s  ", candidatos[i]);
    printf("\n");
    redibujar_linea_pos(linea, *pos);
    return 1;
}

/* Ctrl+R: búsqueda incremental simple */
static int busqueda_reversa(char *linea, size_t tam, size_t *len, size_t *pos) {
    char query[256] = "";
    size_t qlen = 0;
    printf("\r\x1b[2K(reverse-i-search): ");
    fflush(stdout);
    while (1) {
        char c;
        int r = leer_caracter_terminal(&c);
        if (r != 1) {
            redibujar_linea_pos(linea, *pos);
            return 0;
        }
        if (c == '\r' || c == '\n') {
            printf("\n");
            redibujar_linea_pos(linea, *pos);
            return 0;
        }
        if (c == 27) { /* Esc cancela */
            printf("\n");
            redibujar_linea_pos(linea, *pos);
            return 0;
        }
        if (c == 127 || c == '\b') {
            if (qlen > 0) { query[--qlen] = '\0'; }
        } else if (c == 3) { /* Ctrl-C */
            sigint_recibido = 0;
            printf("\n");
            redibujar_linea_pos(linea, *pos);
            return 0;
        } else if (isprint((unsigned char)c) && qlen + 1 < sizeof(query)) {
            query[qlen++] = c;
            query[qlen] = '\0';
        } else {
            continue;
        }
        /* buscar hacia atrás */
        const char *match = NULL;
        for (int i = num_historial - 1; i >= 0; i--) {
            if (strstr(historial[i], query)) { match = historial[i]; break; }
        }
        printf("\r\x1b[2K(reverse-i-search `%s'): %s", query, match ? match : "");
        fflush(stdout);
        if (match) {
            copiar_texto(linea, tam, match);
            *len = strlen(linea);
            *pos = *len;
        }
    }
}

static int leer_linea_basica(char *linea, size_t tam_linea) {
    errno = 0;
    if (fgets(linea, (int)tam_linea, stdin)) {
        if (sigint_recibido) return cancelar_entrada();
        quitar_salto(linea);
        return 1;
    }
    if (ferror(stdin) && errno == EINTR) {
        clearerr(stdin);
        return cancelar_entrada();
    }
    return 0;
}

static int pedir_linea_basica(const char *mensaje, char *linea, size_t tam_linea) {
    while (1) {
        printf("%s", mensaje);
        fflush(stdout);
        int resultado = leer_linea_basica(linea, tam_linea);
        if (resultado != -1) return resultado;
    }
}

static int leer_linea_interactiva(char *linea, size_t tam_linea) {
    if (!isatty(STDIN_FILENO) || !activar_modo_edicion()) {
        return leer_linea_basica(linea, tam_linea);
    }
    linea[0] = '\0';
    char borrador[MAX_LINEA] = "";
    size_t longitud = 0, pos = 0;
    int indice_historial = num_historial;

    while (1) {
        char c;
        int rr = leer_caracter_terminal(&c);
        if (rr != 1) {
            restaurar_terminal();
            if (rr == -1 && sigint_recibido) return cancelar_entrada();
            if (rr == -2) perror("read");
            return 0;
        }
        if (c == '\r' || c == '\n') {
            restaurar_terminal();
            printf("\n");
            fflush(stdout);
            return 1;
        }
        if (c == 4) { /* Ctrl-D */
            if (longitud == 0) { restaurar_terminal(); return 0; }
            if (pos < longitud) {
                memmove(linea + pos, linea + pos + 1, longitud - pos);
                longitud--;
                redibujar_linea_pos(linea, pos);
            }
            continue;
        }
        if (c == 3) { /* Ctrl-C */
            sigint_recibido = 0;
            linea[0] = '\0';
            restaurar_terminal();
            printf("\n");
            fflush(stdout);
            /* reactivar para la siguiente línea */
            if (!activar_modo_edicion()) return -1;
            longitud = 0; pos = 0;
            indice_historial = num_historial;
            borrador[0] = '\0';
            redibujar_linea_pos(linea, pos);
            continue;
        }
        if (c == 26) { /* Ctrl-Z: suspender la shell (ver nota SIGTSTP) */
            restaurar_terminal();
            printf("\n");
            fflush(stdout);
            /* La shell ignora SIGTSTP (job control); hay que ponerlo en
               DFL temporalmente o el raise no la detiene. */
            signal(SIGTSTP, SIG_DFL);
            raise(SIGTSTP);
            /* Al volver con `fg` desde la shell padre, rearmamos. */
            signal(SIGTSTP, SIG_IGN);
            if (!activar_modo_edicion()) return -1;
            redibujar_linea_pos(linea, pos);
            continue;
        }
        if (c == 1) { pos = 0; redibujar_linea_pos(linea, pos); continue; } /* Ctrl-A */
        if (c == 5) { pos = longitud; redibujar_linea_pos(linea, pos); continue; } /* Ctrl-E */
        if (c == 11) { /* Ctrl-K: borrar hasta el final */
            linea[pos] = '\0';
            longitud = pos;
            redibujar_linea_pos(linea, pos);
            continue;
        }
        if (c == 21) { /* Ctrl-U: borrar todo */
            linea[0] = '\0';
            longitud = 0; pos = 0;
            redibujar_linea_pos(linea, pos);
            continue;
        }
        if (c == 23) { /* Ctrl-W: borrar palabra */
            if (pos > 0) {
                size_t fin = pos;
                while (fin > 0 && isspace((unsigned char)linea[fin - 1])) fin--;
                while (fin > 0 && !isspace((unsigned char)linea[fin - 1])) fin--;
                memmove(linea + fin, linea + pos, longitud - pos + 1);
                longitud -= (pos - fin);
                pos = fin;
                redibujar_linea_pos(linea, pos);
            }
            continue;
        }
        if (c == 18) { /* Ctrl-R */
            busqueda_reversa(linea, tam_linea, &longitud, &pos);
            continue;
        }
        if (c == '\t') {
            completar_tab(linea, &longitud, &pos);
            continue;
        }
        if (c == 127 || c == '\b') {
            if (pos > 0) {
                memmove(linea + pos - 1, linea + pos, longitud - pos + 1);
                pos--;
                longitud--;
                redibujar_linea_pos(linea, pos);
            }
            continue;
        }
        if (c == '\x1b') {
            char s, t;
            int r1 = leer_caracter_terminal(&s);
            if (r1 != 1) {
                restaurar_terminal();
                if (r1 == -1 && sigint_recibido) return cancelar_entrada();
                return 0;
            }
            int r2 = leer_caracter_terminal(&t);
            if (r2 != 1) {
                restaurar_terminal();
                if (r2 == -1 && sigint_recibido) return cancelar_entrada();
                return 0;
            }
            if (s == '[') {
                if (t == 'A' && num_historial > 0) { /* up */
                    if (indice_historial == num_historial) copiar_texto(borrador, sizeof(borrador), linea);
                    if (indice_historial > 0) indice_historial--;
                    copiar_texto(linea, tam_linea, historial[indice_historial]);
                    longitud = strlen(linea); pos = longitud;
                    redibujar_linea_pos(linea, pos);
                } else if (t == 'B' && indice_historial < num_historial) { /* down */
                    indice_historial++;
                    if (indice_historial == num_historial) copiar_texto(linea, tam_linea, borrador);
                    else copiar_texto(linea, tam_linea, historial[indice_historial]);
                    longitud = strlen(linea); pos = longitud;
                    redibujar_linea_pos(linea, pos);
                } else if (t == 'C') { /* right: aceptar sugerencia o avanzar */
                    if (pos == longitud && sugerencia_actual[0] != '\0') {
                        size_t sl = strlen(sugerencia_actual);
                        if (longitud + sl < tam_linea) {
                            memcpy(linea + longitud, sugerencia_actual, sl + 1);
                            longitud += sl;
                            pos = longitud;
                        }
                        redibujar_linea_pos(linea, pos);
                    } else if (pos < longitud) {
                        pos++;
                        redibujar_linea_pos(linea, pos);
                    }
                } else if (t == 'D') { /* left */
                    if (pos > 0) { pos--; redibujar_linea_pos(linea, pos); }
                } else if (t == 'H') { pos = 0; redibujar_linea_pos(linea, pos); }
                else if (t == 'F') { pos = longitud; redibujar_linea_pos(linea, pos); }
                else if (t == '3') {
                    char u;
                    int r3 = leer_caracter_terminal(&u);
                    if (r3 == 1 && u == '~' && pos < longitud) {
                        memmove(linea + pos, linea + pos + 1, longitud - pos);
                        longitud--;
                        redibujar_linea_pos(linea, pos);
                    }
                }
            }
            continue;
        }
        if (isprint((unsigned char)c) && longitud + 1 < tam_linea) {
            memmove(linea + pos + 1, linea + pos, longitud - pos + 1);
            linea[pos] = c;
            pos++;
            longitud++;
            redibujar_linea_pos(linea, pos);
        }
    }
}

/* ---------- builtins ---------- */

static void configurar_prompt(void);
static int pedir_valor_config(const char *mensaje, char *destino, size_t tam_destino);

static int cmd_exit(char **argv) {
    int code = ultimo_estado;
    if (argv[1]) {
        char *fin = NULL;
        long v = strtol(argv[1], &fin, 10);
        if (fin != argv[1] && *fin == '\0' && v >= 0 && v <= 255) code = (int)v;
        else { printf("exit: código inválido '%s'\n", argv[1]); ultimo_estado = 1; return 1; }
    }
    exit(code);
    return 0;
}

static int cmd_cd(char **argv) {
    const char *dest = argv[1];
    if (!dest || dest[0] == '\0') {
        const char *home = getenv("HOME");
        if (!home) { printf("cd: HOME no definido\n"); ultimo_estado = 1; return 1; }
        dest = home;
    } else if (dest[0] == '~') {
        static char buf[MAX_RUTA];
        const char *home = getenv("HOME") ? getenv("HOME") : "";
        snprintf(buf, sizeof(buf), "%s%s", home, dest + 1);
        dest = buf;
    }
    if (chdir(dest) != 0) {
        printf("cd: no existe '%s'\n", argv[1] ? argv[1] : "");
        ultimo_estado = 1;
    } else {
        ultimo_estado = 0;
    }
    return 1;
}

static int cmd_help(char **argv) {
    (void)argv;
    printf("Comandos internos:\n");
    printf("  exit                 - salir\n");
    printf("  cd [ruta]            - cambiar de directorio\n");
    printf("  help                 - esta ayuda\n");
    printf("  alias [n cmd]        - crear/actualizar o listar aliases\n");
    printf("  unalias <nombre>     - eliminar un alias\n");
    printf("  config               - editar el prompt\n");
    printf("  echo [args]          - imprimir argumentos\n");
    printf("  pwd                  - mostrar directorio actual\n");
    printf("  export V=valor       - definir variable de entorno\n");
    printf("  unset <var>          - eliminar variable\n");
    printf("  history [-c]         - ver o limpiar historial\n");
    printf("  jobs                 - listar trabajos en segundo plano\n");
    printf("  fg [%%n]              - traer trabajo al frente\n");
    printf("  bg [%%n]              - continuar trabajo detenido\n");
    printf("  wait                 - esperar trabajos en segundo plano\n");
    printf("  true / false / :      - builtins calientes (sin fork)\n");
    printf("  test / [ ... ]       - comparar strings, números y ficheros\n");
    printf("  source <archivo>     - ejecutar script\n");
    printf("  exit [n]             - salir con código n\n");
    printf("Sintaxis: cmd1 | cmd2 ;  cmd > f  >>  <  &  $VAR $? ~  Tab  Ctrl+R\n");
    ultimo_estado = 0;
    return 1;
}

static int cmd_alias(char **argv) {
    if (!argv[1]) {
        if (num_aliases == 0) printf("No hay aliases.\n");
        else {
            printf("Aliases:\n");
            for (int i = 0; i < num_aliases; i++)
                printf("  %-12s -> %s\n", aliases[i].nombre, aliases[i].comando);
        }
        ultimo_estado = 0;
        return 1;
    }
    /* Formas aceptadas: `alias` (listar), `alias n` (ver uno),
       `alias n cmd...` y `alias n=cmd...` estilo bash (las comillas ya
       las quitó el parser: alias gs='git status' llega como gs=git status). */
    char nombre[32] = "";
    copiar_texto(nombre, sizeof(nombre), argv[1]);
    int inicio_cmd = 2;
    char *eq = strchr(nombre, '=');
    char valor_pegado[512] = "";
    if (eq) {
        *eq = '\0';
        copiar_texto(valor_pegado, sizeof(valor_pegado), eq + 1);
        inicio_cmd = 2;
    }
    char comando[512] = "";
    if (valor_pegado[0] != '\0') copiar_texto(comando, sizeof(comando), valor_pegado);
    for (int i = inicio_cmd; argv[i]; i++) {
        if (comando[0] != '\0') strncat(comando, " ", sizeof(comando) - strlen(comando) - 1);
        strncat(comando, argv[i], sizeof(comando) - strlen(comando) - 1);
    }
    if (comando[0] == '\0' && eq == NULL) {
        int idx = buscar_alias(nombre);
        if (idx != -1) printf("%s -> %s\n", aliases[idx].nombre, aliases[idx].comando);
        else printf("alias: no existe '%s'\n", nombre);
        ultimo_estado = idx != -1 ? 0 : 1;
        return 1;
    }
    int indice = buscar_alias(nombre);
    if (indice != -1) {
        copiar_texto(aliases[indice].comando, sizeof(aliases[indice].comando), comando);
        guardar_aliases();
        printf("Alias '%s' actualizado.\n", nombre);
        ultimo_estado = 0;
        return 1;
    }
    if (num_aliases == MAX_ALIAS) {
        printf("Error: limite de aliases alcanzado.\n");
        ultimo_estado = 1;
        return 1;
    }
    copiar_texto(aliases[num_aliases].nombre, sizeof(aliases[num_aliases].nombre), nombre);
    copiar_texto(aliases[num_aliases].comando, sizeof(aliases[num_aliases].comando), comando);
    num_aliases++;
    guardar_aliases();
    printf("Alias '%s' creado.\n", nombre);
    ultimo_estado = 0;
    return 1;
}

static int cmd_unalias(char **argv) {
    if (!argv[1]) { printf("Uso: unalias <nombre>\n"); ultimo_estado = 1; return 1; }
    int idx = buscar_alias(argv[1]);
    if (idx == -1) { printf("unalias: no existe '%s'\n", argv[1]); ultimo_estado = 1; return 1; }
    memmove(&aliases[idx], &aliases[idx + 1], (size_t)(num_aliases - idx - 1) * sizeof(Alias));
    num_aliases--;
    guardar_aliases();
    ultimo_estado = 0;
    return 1;
}

static int cmd_config(char **argv) {
    (void)argv;
    printf("=== EDITAR PROMPT ===\n");
    printf("nombre=%s\nhost=%s\nsimbolo=%s\n\n", prompt_nombre, prompt_host, prompt_simbolo);
    if (pedir_valor_config("Nuevo nombre (enter = mantener): ", prompt_nombre, sizeof(prompt_nombre)) == 0) return 1;
    if (pedir_valor_config("Nuevo host (enter = mantener): ", prompt_host, sizeof(prompt_host)) == 0) return 1;
    if (pedir_valor_config("Nuevo simbolo (enter = mantener): ", prompt_simbolo, sizeof(prompt_simbolo)) == 0) return 1;
    guardar_conf();
    if (persistencia_disponible) printf("Guardado.\n");
    ultimo_estado = 0;
    return 1;
}

static int cmd_echo(char **argv) {
    int newline = 1;
    int i = 1;
    if (argv[1] && strcmp(argv[1], "-n") == 0) { newline = 0; i = 2; }
    for (; argv[i]; i++) {
        if (i > (newline ? 1 : 2)) printf(" ");
        printf("%s", argv[i]);
    }
    if (newline) printf("\n");
    ultimo_estado = 0;
    return 1;
}

static int cmd_pwd(char **argv) {
    (void)argv;
    char buf[MAX_RUTA];
    if (getcwd(buf, sizeof(buf))) { printf("%s\n", buf); ultimo_estado = 0; }
    else { perror("pwd"); ultimo_estado = 1; }
    return 1;
}

static int cmd_export(char **argv) {
    if (!argv[1]) {
        extern char **environ;
        for (char **e = environ; *e; e++) printf("%s\n", *e);
        ultimo_estado = 0;
        return 1;
    }
    for (int i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            /* export VAR sin valor: si no existe, crear vacía */
            if (!getenv(argv[i])) setenv(argv[i], "", 1);
        } else {
            *eq = '\0';
            if (setenv(argv[i], eq + 1, 1) != 0) { perror("export"); ultimo_estado = 1; return 1; }
        }
    }
    ultimo_estado = 0;
    return 1;
}

static int cmd_unset(char **argv) {
    if (!argv[1]) { printf("Uso: unset <var>...\n"); ultimo_estado = 1; return 1; }
    for (int i = 1; argv[i]; i++) unsetenv(argv[i]);
    ultimo_estado = 0;
    return 1;
}

static int cmd_true(char **argv) {
    (void)argv;
    ultimo_estado = 0;
    return 1;
}

static int cmd_false(char **argv) {
    (void)argv;
    ultimo_estado = 1;
    return 1;
}

static int cmd_noop(char **argv) {
    /* `:` de POSIX: expande args (ya hecho por el parser) y siempre ok. */
    (void)argv;
    ultimo_estado = 0;
    return 1;
}

/* `test`/`[` mínimo POSIX: = != -n -z -e -f -d -r -w -x, -eq -ne -gt -ge
   -lt -le, ! expr, expr -a expr, expr -o expr. Sin paréntesis. */
static long test_numero(const char *s, int *ok) {
    char *fin = NULL;
    errno = 0;
    long v = strtol(s, &fin, 10);
    *ok = (errno == 0 && fin != s && *fin == '\0');
    return v;
}

static int test_primaria(int argc, char **argv) {
    if (argc == 0) { ultimo_estado = 1; return 1; }
    if (argc == 1) { ultimo_estado = argv[0][0] == '\0' ? 1 : 0; return 1; }
    if (argc > 1 && strcmp(argv[0], "!") == 0) {
        /* `! expr` vale para cualquier aridad (incluye `! a = b`). */
        test_primaria(argc - 1, argv + 1);
        if (ultimo_estado != 2) ultimo_estado = !ultimo_estado;
        return 1;
    }
    if (argc == 2) {
        if (strcmp(argv[0], "-n") == 0) { ultimo_estado = argv[1][0] ? 0 : 1; return 1; }
        if (strcmp(argv[0], "-z") == 0) { ultimo_estado = argv[1][0] ? 1 : 0; return 1; }
        if (strcmp(argv[0], "-e") == 0) { ultimo_estado = access(argv[1], F_OK) == 0 ? 0 : 1; return 1; }
        if (strcmp(argv[0], "-f") == 0) {
            struct stat st;
            ultimo_estado = (stat(argv[1], &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
            return 1;
        }
        if (strcmp(argv[0], "-d") == 0) {
            struct stat st;
            ultimo_estado = (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
            return 1;
        }
        if (strcmp(argv[0], "-r") == 0) { ultimo_estado = access(argv[1], R_OK) == 0 ? 0 : 1; return 1; }
        if (strcmp(argv[0], "-w") == 0) { ultimo_estado = access(argv[1], W_OK) == 0 ? 0 : 1; return 1; }
        if (strcmp(argv[0], "-x") == 0) { ultimo_estado = access(argv[1], X_OK) == 0 ? 0 : 1; return 1; }
        ultimo_estado = 2;
        return 1;
    }
    if (argc == 3) {
        if (strcmp(argv[1], "=") == 0) { ultimo_estado = strcmp(argv[0], argv[2]) == 0 ? 0 : 1; return 1; }
        if (strcmp(argv[1], "!=") == 0) { ultimo_estado = strcmp(argv[0], argv[2]) != 0 ? 0 : 1; return 1; }
        int ok1 = 0, ok2 = 0;
        long a = test_numero(argv[0], &ok1);
        long b = test_numero(argv[2], &ok2);
        if (ok1 && ok2) {
            if (strcmp(argv[1], "-eq") == 0) ultimo_estado = a == b ? 0 : 1;
            else if (strcmp(argv[1], "-ne") == 0) ultimo_estado = a != b ? 0 : 1;
            else if (strcmp(argv[1], "-gt") == 0) ultimo_estado = a > b ? 0 : 1;
            else if (strcmp(argv[1], "-ge") == 0) ultimo_estado = a >= b ? 0 : 1;
            else if (strcmp(argv[1], "-lt") == 0) ultimo_estado = a < b ? 0 : 1;
            else if (strcmp(argv[1], "-le") == 0) ultimo_estado = a <= b ? 0 : 1;
            else ultimo_estado = 2;
            return 1;
        }
        ultimo_estado = 2;
        return 1;
    }
    ultimo_estado = 2;
    return 1;
}

static int cmd_test(char **argv) {
    int es_corchete = strcmp(argv[0], "[") == 0;
    int argc = 0;
    while (argv[argc + 1]) argc++;
    char **args = argv + 1;
    if (es_corchete) {
        /* `[` exige `]` final. */
        if (argc == 0 || strcmp(argv[argc], "]") != 0) {
            printf("[: falta `]`\n");
            ultimo_estado = 2;
            return 1;
        }
        argc--;
    }
    if (argc > 1) {
        /* -a / -o de baja precedencia: partir por el último -o, si no -a. */
        int sep = -1;
        for (int i = 0; i < argc; i++) {
            if (strcmp(args[i], "-o") == 0) sep = i;
        }
        int es_o = 1;
        if (sep == -1) {
            for (int i = 0; i < argc; i++) {
                if (strcmp(args[i], "-a") == 0) { sep = i; es_o = 0; break; }
            }
        }
        if (sep != -1) {
            /* Evaluar lados con copias temporales terminadas en NULL. */
            char *izq[MAX_ARGS], *der[MAX_ARGS];
            int ni = 0, nd = 0;
            for (int i = 0; i < sep && ni + 1 < MAX_ARGS; i++) izq[ni++] = args[i];
            for (int i = sep + 1; i < argc && nd + 1 < MAX_ARGS; i++) der[nd++] = args[i];
            izq[ni] = NULL; der[nd] = NULL;
            test_primaria(ni, izq);
            int r1 = ultimo_estado;
            test_primaria(nd, der);
            int r2 = ultimo_estado;
            ultimo_estado = es_o ? (r1 == 0 || r2 == 0 ? 0 : 1)
                                 : (r1 == 0 && r2 == 0 ? 0 : 1);
            return 1;
        }
    }
    test_primaria(argc, args);
    return 1;
}

static int cmd_history(char **argv) {
    if (argv[1] && strcmp(argv[1], "-c") == 0) {
        num_historial = 0;
        if (persistencia_disponible) {
            FILE *f = fopen(ruta_historial, "w");
            if (f) fclose(f);
        }
        ultimo_estado = 0;
        return 1;
    }
    for (int i = 0; i < num_historial; i++) printf("%4d  %s\n", i + 1, historial[i]);
    ultimo_estado = 0;
    return 1;
}

/* ---------- jobs ---------- */

/* Recolecta hijos terminados sin bloquear y actualiza la tabla de jobs.
   Nota de diseño: solo guardamos el pgid del grupo. Un pid reaped puede ser
   el líder o un miembro de un pipe; por eso resolvemos el job al que
   pertenece vía getpgid(pid) y, si el grupo ya no existe, lo marcamos
   inactivo. Llamar solo cuando NO se espera en foreground (el wait de
   foreground ya recoge a sus hijos). */
static void limpiar_jobs(void) {
    int estado;
    pid_t pid;
    while ((pid = waitpid(-1, &estado, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        pid_t pg = getpgid(pid);
        for (int i = 0; i < MAX_JOBS; i++) {
            if (!trabajos[i].activo) continue;
            /* El pid es el líder o (si el grupo sigue vivo) un miembro. */
            if (trabajos[i].pgid != pid && (pg == -1 || trabajos[i].pgid != pg)) continue;
            if (WIFEXITED(estado) || WIFSIGNALED(estado)) {
                /* Inactivo solo si no queda nadie en el grupo. */
                if (kill(-trabajos[i].pgid, 0) != 0 && errno == ESRCH) trabajos[i].activo = 0;
            } else if (WIFSTOPPED(estado)) {
                trabajos[i].detenido = 1;
            } else if (WIFCONTINUED(estado)) {
                trabajos[i].detenido = 0;
            }
        }
    }
}

static int buscar_job(const char *arg) {
    if (!arg) {
        /* el más reciente activo */
        int mejor = -1;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (trabajos[i].activo && (mejor == -1 || trabajos[i].id > trabajos[mejor].id)) mejor = i;
        }
        return mejor;
    }
    const char *p = arg;
    if (*p == '%') p++;
    char *fin = NULL;
    long id = strtol(p, &fin, 10);
    if (fin != p && *fin == '\0') {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (trabajos[i].activo && trabajos[i].id == (int)id) return i;
        }
        return -1;
    }
    /* buscar por pid */
    pid_t pid = (pid_t)id;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (trabajos[i].activo && trabajos[i].pgid == pid) return i;
    }
    return -1;
}

static int agregar_job(pid_t pgid, const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!trabajos[i].activo) {
            trabajos[i].activo = 1;
            trabajos[i].detenido = 0;
            trabajos[i].pgid = pgid;
            trabajos[i].id = siguiente_job_id++;
            copiar_texto(trabajos[i].cmd, sizeof(trabajos[i].cmd), cmd);
            return trabajos[i].id;
        }
    }
    return -1;
}

static int cmd_jobs(char **argv) {
    (void)argv;
    limpiar_jobs();
    for (int i = 0; i < MAX_JOBS; i++) {
        if (trabajos[i].activo) {
            printf("[%d] %s %d  %s\n", trabajos[i].id,
                   trabajos[i].detenido ? "Detenido" : "Ejecutando",
                   (int)trabajos[i].pgid, trabajos[i].cmd);
        }
    }
    ultimo_estado = 0;
    return 1;
}

static int esperar_foreground(pid_t pgid, pid_t ultimo_pid) {
    int estado = 0;
    int estado_final = 0;
    int visto_final = 0;
    pid_t w;
    tcsetpgrp(STDIN_FILENO, pgid);
    while ((w = waitpid(-pgid, &estado, WUNTRACED)) != 0) {
        if (w == -1) {
            if (errno == EINTR) continue; /* señal a la shell: seguir esperando */
            break;
        }
        /* POSIX: el estado del pipeline es el del último comando. */
        if (ultimo_pid == -1 || w == ultimo_pid) {
            estado_final = estado;
            visto_final = 1;
        }
        if (WIFSTOPPED(estado)) { estado_final = estado; break; }
        if (WIFEXITED(estado) || WIFSIGNALED(estado)) {
            /* si no quedan procesos del grupo, salir */
            if (kill(-pgid, 0) != 0) break;
            /* puede haber más hijos del mismo pipe */
            continue;
        }
    }
    if (visto_final) estado = estado_final;
    tcsetpgrp(STDIN_FILENO, shell_pgid);
    if (WIFEXITED(estado)) ultimo_estado = WEXITSTATUS(estado);
    else if (WIFSIGNALED(estado)) ultimo_estado = 128 + WTERMSIG(estado);
    else if (WIFSTOPPED(estado)) ultimo_estado = 128 + WSTOPSIG(estado);
    sigint_recibido = 0;
    return estado;
}

static int cmd_fg(char **argv) {
    limpiar_jobs();
    int idx = buscar_job(argv[1]);
    if (idx == -1) { printf("fg: no hay ese trabajo\n"); ultimo_estado = 1; return 1; }
    pid_t pgid = trabajos[idx].pgid;
    printf("%s\n", trabajos[idx].cmd);
    kill(-pgid, SIGCONT);
    trabajos[idx].detenido = 0;
    int estado = esperar_foreground(pgid, -1);
    if (WIFSTOPPED(estado)) {
        trabajos[idx].detenido = 1;
        printf("\n[%d]+ Detenido  %s\n", trabajos[idx].id, trabajos[idx].cmd);
    } else {
        trabajos[idx].activo = 0;
    }
    return 1;
}

static int cmd_bg(char **argv) {
    limpiar_jobs();
    int idx = buscar_job(argv[1]);
    if (idx == -1) { printf("bg: no hay ese trabajo\n"); ultimo_estado = 1; return 1; }
    kill(-trabajos[idx].pgid, SIGCONT);
    trabajos[idx].detenido = 0;
    printf("[%d] %s\n", trabajos[idx].id, trabajos[idx].cmd);
    ultimo_estado = 0;
    return 1;
}

static int cmd_wait(char **argv) {
    (void)argv;
    int estado = 0;
    pid_t w;
    ultimo_estado = 0;
    while ((w = waitpid(-1, &estado, 0)) != 0) {
        if (w == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (WIFEXITED(estado)) ultimo_estado = WEXITSTATUS(estado);
        else if (WIFSIGNALED(estado)) ultimo_estado = 128 + WTERMSIG(estado);
        for (int i = 0; i < MAX_JOBS; i++) {
            if (trabajos[i].activo && (trabajos[i].pgid == w || kill(-trabajos[i].pgid, 0) != 0))
                trabajos[i].activo = 0;
        }
    }
    limpiar_jobs();
    return 1;
}

static void ejecutar_linea(char *linea);

static int profundidad_source = 0;

static int cmd_source(char **argv) {
    if (!argv[1]) { printf("Uso: source <archivo>\n"); ultimo_estado = 1; return 1; }
    if (profundidad_source >= MAX_SOURCE_DEPTH) {
        printf("source: profundidad máxima (%d), posible recursión\n", MAX_SOURCE_DEPTH);
        ultimo_estado = 1;
        return 1;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("source"); ultimo_estado = 1; return 1; }
    profundidad_source++;
    char linea[MAX_LINEA];
    while (fgets(linea, sizeof(linea), f)) {
        quitar_salto(linea);
        char *t = recortar(linea);
        if (t[0] == '\0' || t[0] == '#') continue;
        char copia[MAX_LINEA];
        copiar_texto(copia, sizeof(copia), t);
        ejecutar_linea(copia);
    }
    fclose(f);
    profundidad_source--;
    return 1;
}

static int es_builtin(const char *nombre) {
    for (int i = 0; comandos[i].nombre; i++) {
        if (strcmp(nombre, comandos[i].nombre) == 0) return i;
    }
    return -1;
}

/* ---------- parsing + expansión ---------- */

static int pedir_valor_config(const char *mensaje, char *destino, size_t tam_destino) {
    char temporal[MAX_LINEA];
    int resultado = pedir_linea_basica(mensaje, temporal, sizeof(temporal));
    if (resultado == 1 && temporal[0] != '\0') copiar_texto(destino, tam_destino, temporal);
    return resultado;
}

static void configurar_prompt(void) {
    if (cargar_conf()) return;
    /* no preguntar si no es interactivo (más rápido en scripts/tests) */
    if (!isatty(STDIN_FILENO)) return;
    printf("=== QUANTIC SHELL - CONFIGURACION INICIAL ===\n");
    if (pedir_linea_basica("Nombre (enter = usuario): ", prompt_nombre, sizeof(prompt_nombre)) == 0) return;
    if (prompt_nombre[0] == '\0') {
        copiar_texto(prompt_nombre, sizeof(prompt_nombre), getenv("USER") ? getenv("USER") : "usuario");
    }
    if (pedir_linea_basica("Host (enter = hostname): ", prompt_host, sizeof(prompt_host)) == 0) return;
    if (prompt_host[0] == '\0') {
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) == -1) copiar_texto(hostname, sizeof(hostname), "localhost");
        hostname[sizeof(prompt_host) - 1] = '\0';
        copiar_texto(prompt_host, sizeof(prompt_host), hostname);
    }
    if (pedir_linea_basica("Simbolo (enter = $): ", prompt_simbolo, sizeof(prompt_simbolo)) == 0) return;
    if (prompt_simbolo[0] == '\0') copiar_texto(prompt_simbolo, sizeof(prompt_simbolo), "$");
    guardar_conf();
    if (persistencia_disponible) printf("\nConfiguracion guardada en %s\n\n", ruta_conf);
}

/* Divide por ';' respetando comillas y escapes. Modifica el buffer con '\0'.
   Devuelve número de segmentos en `partes` (punteros dentro de buf). */
static int dividir_puntoycoma(char *buf, char *partes[], int max) {
    int n = 0;
    char comilla = '\0';
    int esc = 0;
    char *inicio = buf;
    for (char *p = buf; ; p++) {
        char c = *p;
        if (c == '\0') {
            if (n < max) partes[n++] = inicio;
            return n;
        }
        if (esc) { esc = 0; continue; }
        if (c == '\\' && comilla != '\'') { esc = 1; continue; }
        if (comilla) { if (c == comilla) comilla = '\0'; continue; }
        if (c == '\'' || c == '"') { comilla = c; continue; }
        if (c == ';') {
            *p = '\0';
            if (n < max) partes[n++] = inicio;
            inicio = p + 1;
        }
    }
}

static int dividir_pipes(char *buf, char *partes[], int max) {
    int n = 0;
    char comilla = '\0';
    int esc = 0;
    char *inicio = buf;
    for (char *p = buf; ; p++) {
        char c = *p;
        if (c == '\0') {
            if (n < max) partes[n++] = inicio;
            return n;
        }
        if (esc) { esc = 0; continue; }
        if (c == '\\' && comilla != '\'') { esc = 1; continue; }
        if (comilla) { if (c == comilla) comilla = '\0'; continue; }
        if (c == '\'' || c == '"') { comilla = c; continue; }
        if (c == '|') {
            *p = '\0';
            if (n < max) partes[n++] = inicio;
            inicio = p + 1;
        }
    }
}

/* Detecta '&' final no entrecomillado (background). Lo elimina y devuelve 1. */
static int extraer_background(char *texto) {
    char comilla = '\0';
    int esc = 0;
    char *ultimo_amp = NULL;
    for (char *p = texto; ; p++) {
        char c = *p;
        if (c == '\0') break;
        if (esc) { esc = 0; continue; }
        if (c == '\\' && comilla != '\'') { esc = 1; continue; }
        if (comilla) { if (c == comilla) comilla = '\0'; continue; }
        if (c == '\'' || c == '"') { comilla = c; continue; }
        if (c == '&') ultimo_amp = p;
        else if (!isspace((unsigned char)c)) ultimo_amp = NULL; /* solo vale si es lo último */
    }
    if (ultimo_amp) {
        /* verificar que después solo hay espacios */
        for (char *p = ultimo_amp + 1; *p; p++) {
            if (!isspace((unsigned char)*p)) return 0;
        }
        *ultimo_amp = '\0';
        return 1;
    }
    return 0;
}

/* Expande $VAR, ${VAR}, $?, $$ y ~ inicial en un token.
   `permitir_glob` se pone a 1 si el token contenía *?intersincomillar. */
static void expandir_token(const char *src, char *dst, size_t tam, int entre_simple, int *tuvo_glob) {
    size_t di = 0;
    if (tuvo_glob) *tuvo_glob = 0;
    for (size_t i = 0; src[i] && di + 1 < tam; ) {
        if (!entre_simple && src[i] == '~' && i == 0 && (src[1] == '/' || src[1] == '\0')) {
            const char *home = getenv("HOME") ? getenv("HOME") : "";
            while (*home && di + 1 < tam) dst[di++] = *home++;
            i++;
            continue;
        }
        if (!entre_simple && src[i] == '$') {
            if (src[i+1] == '?') {
                char num[16];
                snprintf(num, sizeof(num), "%d", ultimo_estado);
                for (char *p = num; *p && di + 1 < tam; p++) dst[di++] = *p;
                i += 2;
                continue;
            }
            if (src[i+1] == '$') {
                char num[32];
                snprintf(num, sizeof(num), "%d", (int)getpid());
                for (char *p = num; *p && di + 1 < tam; p++) dst[di++] = *p;
                i += 2;
                continue;
            }
            if (src[i+1] == '{') {
                size_t j = i + 2;
                while (src[j] && src[j] != '}') j++;
                char var[128];
                size_t vl = j - (i + 2);
                if (vl >= sizeof(var)) vl = sizeof(var) - 1;
                memcpy(var, src + i + 2, vl);
                var[vl] = '\0';
                const char *val = getenv(var);
                if (val) while (*val && di + 1 < tam) dst[di++] = *val++;
                i = src[j] == '}' ? j + 1 : j;
                continue;
            }
            if (isalpha((unsigned char)src[i+1]) || src[i+1] == '_') {
                size_t j = i + 1;
                while (isalnum((unsigned char)src[j]) || src[j] == '_') j++;
                char var[128];
                size_t vl = j - (i + 1);
                if (vl >= sizeof(var)) vl = sizeof(var) - 1;
                memcpy(var, src + i + 1, vl);
                var[vl] = '\0';
                const char *val = getenv(var);
                if (val) while (*val && di + 1 < tam) dst[di++] = *val++;
                i = j;
                continue;
            }
            /* '$' suelto */
            dst[di++] = src[i++];
            continue;
        }
        if (!entre_simple && (src[i] == '*' || src[i] == '?' || src[i] == '[') && tuvo_glob) {
            *tuvo_glob = 1;
        }
        dst[di++] = src[i++];
    }
    dst[di] = '\0';
}

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *entrada;
    char *salida;
    int append;
} ComandoParseado;

static void liberar_parseado(ComandoParseado *cp) {
    for (int i = 0; i < cp->argc; i++) free(cp->argv[i]);
    if (cp->entrada) free(cp->entrada);
    if (cp->salida) free(cp->salida);
    cp->argc = 0;
    cp->entrada = cp->salida = NULL;
}

/* Tokeniza un tramo de pipe en argv + redirecciones, con comillas, expansión y glob.
   Devuelve 0 si ok, -1 si error. */
static int parsear_tramo(char *tramo, ComandoParseado *out) {
    memset(out, 0, sizeof(*out));
    /* buffers temporales */
    char token[MAX_LINEA];
    size_t tl = 0;
    char comilla = '\0';
    int esc = 0;
    int en_token = 0;
    int token_simple = 0;   /* 1 si todo el token fue entre comillas simples */
    int token_doble = 0;
    int token_tuvo_comilla = 0;

    char *tokens_crudo[MAX_ARGS];
    int flags_simple[MAX_ARGS];
    int n_crudo = 0;

    for (char *p = tramo; ; p++) { /* NOLINT: recorre hasta el '\0' final */
        char c = *p;
        int fin = (c == '\0');
        if (esc) {
            if (!en_token) { en_token = 1; tl = 0; token_simple = 0; token_doble = 0; token_tuvo_comilla = 0; }
            if (tl + 1 < sizeof(token)) token[tl++] = c;
            esc = 0;
            if (fin) break;
            continue;
        }
        if (!comilla && c == '\\') { esc = 1; if (fin) break; continue; }
        if (comilla) {
            if (c == comilla) { comilla = '\0'; token_tuvo_comilla = 1; }
            else if (fin) break;
            else {
                if (!en_token) { en_token = 1; tl = 0; token_simple = (comilla == '\''); token_doble = (comilla == '"'); }
                if (tl + 1 < sizeof(token)) token[tl++] = c;
            }
            if (fin) break;
            continue;
        }
        if (c == '\'' || c == '"') {
            if (!en_token) { en_token = 1; tl = 0; token_simple = (c == '\''); token_doble = (c == '"'); }
            else {
                if (c == '\'' && !token_doble) token_simple = 1;
                if (c == '"') token_doble = 1;
            }
            comilla = c;
            token_tuvo_comilla = 1;
            if (fin) break;
            continue;
        }
        if (fin || isspace((unsigned char)c)) {
            if (en_token) {
                token[tl] = '\0';
                if (n_crudo >= MAX_ARGS) {
                    printf("Error: demasiados argumentos (máx %d)\n", MAX_ARGS);
                    for (int k = 0; k < n_crudo; k++) free(tokens_crudo[k]);
                    liberar_parseado(out);
                    return -1;
                }
                if (n_crudo < MAX_ARGS) {
                    tokens_crudo[n_crudo] = malloc(tl + 1);
                    if (!tokens_crudo[n_crudo]) { perror("malloc"); return -1; }
                    memcpy(tokens_crudo[n_crudo], token, tl + 1);
                    flags_simple[n_crudo] = (token_simple && !token_doble);
                    n_crudo++;
                }
                en_token = 0; tl = 0;
            }
            if (fin) break;
            continue;
        }
        if (!en_token) { en_token = 1; tl = 0; token_simple = 0; token_doble = 0; token_tuvo_comilla = 0; }
        if (tl + 1 < sizeof(token)) token[tl++] = c;
    }
    (void)token_tuvo_comilla;

    /* procesar redirecciones y expansión */
    for (int i = 0; i < n_crudo; i++) {
        if (strcmp(tokens_crudo[i], ">") == 0 || strcmp(tokens_crudo[i], ">>") == 0 || strcmp(tokens_crudo[i], "<") == 0) {
            int es_append = strcmp(tokens_crudo[i], ">>") == 0;
            int es_entrada = strcmp(tokens_crudo[i], "<") == 0;
            free(tokens_crudo[i]);
            if (i + 1 >= n_crudo) {
                printf("Error de sintaxis: falta archivo tras redirección\n");
                for (int k = i + 1; k < n_crudo; k++) free(tokens_crudo[k]);
                liberar_parseado(out);
                return -1;
            }
            char expandido[MAX_LINEA];
            int g = 0;
            expandir_token(tokens_crudo[i + 1], expandido, sizeof(expandido), 0, &g);
            if (es_entrada) { out->entrada = malloc(strlen(expandido) + 1); if (out->entrada) strcpy(out->entrada, expandido); }
            else { out->salida = malloc(strlen(expandido) + 1); if (out->salida) strcpy(out->salida, expandido); out->append = es_append; }
            free(tokens_crudo[i + 1]);
            i++;
            continue;
        }
        /* chequear redirección pegada: prefijo >, >>, < */
        if (!flags_simple[i] && (tokens_crudo[i][0] == '>' || tokens_crudo[i][0] == '<') && tokens_crudo[i][1] != '\0') {
            char op = tokens_crudo[i][0];
            int es_append = (op == '>' && tokens_crudo[i][1] == '>');
            const char *nombre = tokens_crudo[i] + (es_append ? 2 : 1);
            if (nombre[0] == '\0') {
                /* operador solo + siguiente token */
                free(tokens_crudo[i]);
                if (i + 1 >= n_crudo) {
                    printf("Error de sintaxis: falta archivo tras redirección\n");
                    for (int k = i + 1; k < n_crudo; k++) free(tokens_crudo[k]);
                    liberar_parseado(out);
                    return -1;
                }
                char expandido[MAX_LINEA];
                int g = 0;
                expandir_token(tokens_crudo[i + 1], expandido, sizeof(expandido), 0, &g);
                if (op == '<') { out->entrada = malloc(strlen(expandido) + 1); if (out->entrada) strcpy(out->entrada, expandido); }
                else { out->salida = malloc(strlen(expandido) + 1); if (out->salida) strcpy(out->salida, expandido); out->append = es_append; }
                free(tokens_crudo[i + 1]);
                i++;
                continue;
            }
            char expandido[MAX_LINEA];
            int g = 0;
            expandir_token(nombre, expandido, sizeof(expandido), 0, &g);
            if (op == '<') { out->entrada = malloc(strlen(expandido) + 1); if (out->entrada) strcpy(out->entrada, expandido); }
            else { out->salida = malloc(strlen(expandido) + 1); if (out->salida) strcpy(out->salida, expandido); out->append = es_append; }
            free(tokens_crudo[i]);
            continue;
        }
        /* argumento normal: expandir variables */
        char expandido[MAX_LINEA];
        int tuvo_glob = 0;
        expandir_token(tokens_crudo[i], expandido, sizeof(expandido), flags_simple[i], &tuvo_glob);
        free(tokens_crudo[i]);
        if (tuvo_glob && !flags_simple[i]) {
            glob_t g;
            memset(&g, 0, sizeof(g));
            int r = glob(expandido, GLOB_NOCHECK, NULL, &g);
            if (r == 0) {
                for (size_t k = 0; k < g.gl_pathc && out->argc + 1 < MAX_ARGS; k++) {
                    out->argv[out->argc] = malloc(strlen(g.gl_pathv[k]) + 1);
                    if (out->argv[out->argc]) { strcpy(out->argv[out->argc], g.gl_pathv[k]); out->argc++; }
                }
                globfree(&g);
                continue;
            }
            globfree(&g);
        }
        if (out->argc + 1 < MAX_ARGS) {
            out->argv[out->argc] = malloc(strlen(expandido) + 1);
            if (out->argv[out->argc]) { strcpy(out->argv[out->argc], expandido); out->argc++; }
        }
    }
    out->argv[out->argc] = NULL;
    return 0;
}

/* Ejecuta un builtin con redirecciones aplicadas (solo proceso padre) */
static int ejecutar_builtin_padre(ComandoParseado *cp, int idx_builtin) {
    /* Vaciar ANTES de redirigir: si no, la salida pendiente anterior
       (p.ej. el "[1] pid" de un job) se volcaría al fichero redirigido. */
    fflush(stdout);
    fflush(stderr);
    int save_in = -1, save_out = -1;
    if (cp->entrada) {
        int fd = open(cp->entrada, O_RDONLY);
        if (fd < 0) { perror(cp->entrada); ultimo_estado = 1; return 1; }
        save_in = dup(STDIN_FILENO);
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (cp->salida) {
        int fd = open(cp->salida, O_WRONLY | O_CREAT | (cp->append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { perror(cp->salida); ultimo_estado = 1; if (save_in != -1) { dup2(save_in, STDIN_FILENO); close(save_in); } return 1; }
        save_out = dup(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    comandos[idx_builtin].funcion(cp->argv);
    fflush(stdout);
    if (save_in != -1) { dup2(save_in, STDIN_FILENO); close(save_in); }
    if (save_out != -1) { dup2(save_out, STDOUT_FILENO); close(save_out); }
    return 1;
}

/* ¿El texto contiene sintaxis avanzada que delegamos a /bin/sh? */
static int necesita_sh(const char *texto) {
    char comilla = '\0';
    int esc = 0;
    for (const char *p = texto; *p; p++) {
        if (esc) { esc = 0; continue; }
        if (*p == '\\' && comilla != '\'') { esc = 1; continue; }
        if (comilla) { if (*p == comilla) comilla = '\0'; continue; }
        if (*p == '\'' || *p == '"') { comilla = *p; continue; }
        if (*p == '$' && p[1] == '(') return 1; /* $(...): sustitución, a sh */
        if (*p == '$' && p[1] == '{') {
            /* ${VAR} simple es nativo; ${VAR:-def}, ${#..} etc. van a sh. */
            const char *q = p + 2;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
            if (*q != '}') return 1;
        }
        if (*p == '`') return 1;
        if (*p == '&' && p[1] == '&') return 1;
        if (*p == '|' && p[1] == '|') return 1;
    }
    return 0;
}

static void ejecutar_con_sh(const char *texto, int background, const char *cmd_original) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == -1) { perror("fork"); ultimo_estado = 1; return; }
    if (pid == 0) {
        setpgid(0, 0);
        restaurar_senales_para_hijo();
        if (!background) tcsetpgrp(STDIN_FILENO, getpid());
        execl("/bin/sh", "sh", "-c", texto, (char *)NULL);
        perror("/bin/sh");
        _exit(127);
    }
    setpgid(pid, pid);
    if (background) {
        int id = agregar_job(pid, cmd_original);
        if (id != -1) printf("[%d] %d\n", id, (int)pid);
        ultimo_estado = 0;
        return;
    }
    int estado = esperar_foreground(pid, pid);
    if (WIFSTOPPED(estado)) {
        int id = agregar_job(pid, cmd_original);
        if (id != -1) printf("\n[%d]+ Detenido  %s\n", id, cmd_original);
    }
}

/* Pipeline nativo: tramos ya parseados */
static void ejecutar_pipeline(ComandoParseado *cmds, int n, int background, const char *texto_original) {
    /* caso rápido: un solo comando builtin sin pipe -> en el padre */
    if (n == 1 && cmds[0].argc > 0) {
        int bi = es_builtin(cmds[0].argv[0]);
        if (bi >= 0 && !background) {
            ejecutar_builtin_padre(&cmds[0], bi);
            return;
        }
        if (bi >= 0 && background) {
            /* builtin en background: fork para no bloquear */
            fflush(stdout);
            fflush(stderr);
            pid_t pid = fork();
            if (pid == 0) {
                setpgid(0, 0);
                restaurar_senales_para_hijo();
                if (cmds[0].entrada) {
                    int fd = open(cmds[0].entrada, O_RDONLY);
                    if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
                }
                if (cmds[0].salida) {
                    int fd = open(cmds[0].salida, O_WRONLY | O_CREAT | (cmds[0].append ? O_APPEND : O_TRUNC), 0644);
                    if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
                }
                comandos[bi].funcion(cmds[0].argv);
                fflush(stdout);
                _exit(ultimo_estado);
            }
            setpgid(pid, pid);
            int id = agregar_job(pid, texto_original);
            if (id != -1) printf("[%d] %d\n", id, (int)pid);
            ultimo_estado = 0;
            return;
        }
    }
    /* si hay builtin en medio de un pipe, lo ejecutamos en hijo igualmente */
    int tubos[MAX_TRAMOS_PIPE][2];
    for (int i = 0; i < n - 1; i++) {
        if (pipe(tubos[i]) == -1) { perror("pipe"); ultimo_estado = 1; return; }
    }
    pid_t pgid = 0;
    pid_t pids[MAX_TRAMOS_PIPE];
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < n; i++) {
        if (cmds[i].argc == 0) { pids[i] = -1; continue; }
        pid_t pid = fork();
        if (pid == -1) { perror("fork"); ultimo_estado = 1; return; }
        if (pid == 0) {
            /* hijo */
            if (i == 0) setpgid(0, 0);
            else setpgid(0, pgid);
            restaurar_senales_para_hijo();
            if (!background) tcsetpgrp(STDIN_FILENO, getpgid(0));
            /* pipes */
            if (i > 0) dup2(tubos[i-1][0], STDIN_FILENO);
            if (i < n - 1) dup2(tubos[i][1], STDOUT_FILENO);
            for (int k = 0; k < n - 1; k++) { close(tubos[k][0]); close(tubos[k][1]); }
            /* redirecciones */
            if (cmds[i].entrada) {
                int fd = open(cmds[i].entrada, O_RDONLY);
                if (fd < 0) { perror(cmds[i].entrada); _exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (cmds[i].salida) {
                int fd = open(cmds[i].salida, O_WRONLY | O_CREAT | (cmds[i].append ? O_APPEND : O_TRUNC), 0644);
                if (fd < 0) { perror(cmds[i].salida); _exit(1); }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            int bi = es_builtin(cmds[i].argv[0]);
            if (bi >= 0) {
                comandos[bi].funcion(cmds[i].argv);
                fflush(stdout);
                fflush(stderr);
                _exit(ultimo_estado);
            }
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "quanticshell: comando no encontrado: %s\n", cmds[i].argv[0]);
            _exit(127);
        }
        /* padre */
        if (i == 0) pgid = pid;
        setpgid(pid, pgid);
        pids[i] = pid;
    }
    for (int k = 0; k < n - 1; k++) { close(tubos[k][0]); close(tubos[k][1]); }
    (void)pids;
    if (background) {
        int id = agregar_job(pgid, texto_original);
        if (id != -1) printf("[%d] %d\n", id, (int)pgid);
        ultimo_estado = 0;
        return;
    }
    int estado = esperar_foreground(pgid, (n > 0 && pids[n - 1] != -1) ? pids[n - 1] : -1);
    /* recoger el resto de hijos del pipe */
    {
        int st;
        pid_t w;
        while ((w = waitpid(-pgid, &st, WNOHANG)) > 0) { (void)w; (void)st; }
    }
    if (WIFSTOPPED(estado)) {
        int id = agregar_job(pgid, texto_original);
        if (id != -1) printf("\n[%d]+ Detenido  %s\n", id, texto_original);
    }
}

/* Expande alias del primer token (1 nivel de recursión, máx 8) */
static int expandir_alias_linea(char *linea, size_t tam) {
    for (int depth = 0; depth < 8; depth++) {
        char *t = recortar(linea);
        if (t != linea) memmove(linea, t, strlen(t) + 1);
        /* extraer primera palabra */
        char nombre[64] = "";
        size_t i = 0;
        while (linea[i] && !isspace((unsigned char)linea[i]) && linea[i] != '|' && linea[i] != ';' && linea[i] != '&' && linea[i] != '>' && linea[i] != '<' && i + 1 < sizeof(nombre)) {
            nombre[i] = linea[i];
            i++;
        }
        nombre[i] = '\0';
        if (nombre[0] == '\0') return 0;
        int idx = buscar_alias(nombre);
        if (idx == -1) return 0;
        const char *resto = linea + i;
        char nueva[MAX_LINEA];
        int n = snprintf(nueva, sizeof(nueva), "%s%s", aliases[idx].comando, resto);
        if (n < 0 || (size_t)n >= sizeof(nueva)) return 0;
        copiar_texto(linea, tam, nueva);
    }
    return 0;
}

static void ejecutar_segmento(char *segmento_raw) {
    char *texto = recortar(segmento_raw);
    if (texto[0] == '\0') return;

    char linea[MAX_LINEA];
    copiar_texto(linea, sizeof(linea), texto);
    char original[MAX_LINEA];
    copiar_texto(original, sizeof(original), texto);
    /* display sin '&' final para jobs */
    {
        char *f = original + strlen(original);
        while (f > original && isspace((unsigned char)f[-1])) *--f = '\0';
        if (f > original && f[-1] == '&') {
            *--f = '\0';
            while (f > original && isspace((unsigned char)f[-1])) *--f = '\0';
        }
    }

    expandir_alias_linea(linea, sizeof(linea));

    if (necesita_sh(linea)) {
        /* delegar sintaxis avanzada a sh (compatibilidad), pero sin bloquear mejoras */
        int bg = extraer_background(linea);
        ejecutar_con_sh(linea, bg, original);
        return;
    }

    int background = extraer_background(linea);
    char *tramos[MAX_TRAMOS_PIPE];
    int n_tramos = dividir_pipes(linea, tramos, MAX_TRAMOS_PIPE);
    if (n_tramos <= 0) return;

    ComandoParseado cmds[MAX_TRAMOS_PIPE];
    memset(cmds, 0, sizeof(cmds));
    int n_validos = 0;
    for (int i = 0; i < n_tramos; i++) {
        char *t = recortar(tramos[i]);
        if (t[0] == '\0') continue;
        if (parsear_tramo(t, &cmds[n_validos]) != 0) {
            for (int k = 0; k < n_validos; k++) liberar_parseado(&cmds[k]);
            ultimo_estado = 1;
            return;
        }
        if (cmds[n_validos].argc == 0 && !cmds[n_validos].entrada && !cmds[n_validos].salida) {
            liberar_parseado(&cmds[n_validos]);
            continue;
        }
        n_validos++;
    }
    if (n_validos == 0) return;
    ejecutar_pipeline(cmds, n_validos, background, original);
    for (int i = 0; i < n_validos; i++) liberar_parseado(&cmds[i]);
}

static void ejecutar_linea(char *linea) {
    char copia[MAX_LINEA];
    copiar_texto(copia, sizeof(copia), linea);
    char *partes[64];
    int n = dividir_puntoycoma(copia, partes, 64);
    for (int i = 0; i < n; i++) {
        ejecutar_segmento(partes[i]);
        limpiar_jobs();
    }
}

static void cargar_rc(void) {
    if (!persistencia_disponible) return;
    FILE *f = fopen(ruta_rc, "r");
    if (!f) return;
    char linea[MAX_LINEA];
    while (fgets(linea, sizeof(linea), f)) {
        quitar_salto(linea);
        char *t = recortar(linea);
        if (t[0] == '\0' || t[0] == '#') continue;
        char copia[MAX_LINEA];
        copiar_texto(copia, sizeof(copia), t);
        /* el rc no debe cambiar el ultimo_estado de forma visible? sí lo cambia, es normal */
        ejecutar_linea(copia);
    }
    fclose(f);
    ultimo_estado = 0;
}

static int uso(const char *prog) {
    printf("Uso: %s [-c comando] [--version] [--help]\n", prog);
    return 0;
}

int main(int argc, char **argv) {
    const char *comando_c = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("quanticshell %s\n", QUANTIC_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) return uso(argv[0]);
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            comando_c = argv[++i];
        } else {
            return uso(argv[0]);
        }
    }
    /* job control de la shell misma */
    shell_pgid = getpid();
    if (isatty(STDIN_FILENO)) {
        /* ignorar para poder hacer tcsetpgrp sin pararnos */
        signal(SIGTTOU, SIG_IGN);
        setpgid(shell_pgid, shell_pgid);
        tcsetpgrp(STDIN_FILENO, shell_pgid);
    }

    inicializar_rutas();
    instalar_manejadores();
    atexit(restaurar_terminal);

    configurar_prompt();
    cargar_aliases();
    cargar_historial();
    cargar_rc();

    /* Modo no interactivo de un solo comando: ideal para scripts y tests. */
    if (comando_c) {
        char copia[MAX_LINEA];
        copiar_texto(copia, sizeof(copia), comando_c);
        ejecutar_linea(copia);
        return ultimo_estado;
    }

    /* Sin tty en stdin (pipe/script): sin banner ni prompts, salida limpia.
       Sin tty en stdout o con NO_COLOR: sin secuencias ANSI. */
    int entrada_tty = isatty(STDIN_FILENO);
    usar_color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;

    if (entrada_tty) {
        if (usar_color) printf("\x1b[1m=== QUANTIC SHELL v%s ===\x1b[0m\n", QUANTIC_VERSION);
        else printf("=== QUANTIC SHELL v%s ===\n", QUANTIC_VERSION);
    }

    char linea[MAX_LINEA];
    while (1) {
        limpiar_jobs();
        if (entrada_tty) mostrar_prompt();
        int resultado = leer_linea_interactiva(linea, sizeof(linea));
        if (resultado == 0) break;
        if (resultado == -1) continue;
        if (linea[0] == '\0') continue;
        if (entrada_tty) agregar_historial(linea);
        ejecutar_linea(linea);
    }
    if (entrada_tty) printf("\nAdios.\n");
    return ultimo_estado;
}
