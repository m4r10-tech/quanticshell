#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_ALIAS 64
#define MAX_LINEA 1024
#define MAX_HISTORIAL 100
#define MAX_RUTA 1024

#define NOMBRE_CONF "config"
#define NOMBRE_ALIASES "aliases"
#define NOMBRE_HISTORIAL "history"

typedef struct {
    char nombre[32];
    char comando[256];
} Alias;

typedef struct {
    const char *nombre;
    void (*funcion)(char *args);
} Comando;

Alias aliases[MAX_ALIAS];
int num_aliases = 0;

char historial[MAX_HISTORIAL][MAX_LINEA];
int num_historial = 0;

char prompt_nombre[32] = "usuario";
char prompt_host[32] = "localhost";
char prompt_simbolo[4] = "$";

char ruta_conf[MAX_RUTA];
char ruta_aliases[MAX_RUTA];
char ruta_historial[MAX_RUTA];
int persistencia_disponible = 0;

volatile sig_atomic_t sigint_recibido = 0;
struct termios terminal_original;
int terminal_activo = 0;

void copiar_texto(char *destino, size_t tam_destino, const char *origen) {
    if (tam_destino == 0) return;
    snprintf(destino, tam_destino, "%s", origen ? origen : "");
}

void quitar_salto(char *texto) {
    texto[strcspn(texto, "\n")] = '\0';
}

char *recortar(char *texto) {
    while (*texto && isspace((unsigned char)*texto)) texto++;

    char *fin = texto + strlen(texto);
    while (fin > texto && isspace((unsigned char)fin[-1])) {
        *--fin = '\0';
    }

    return texto;
}

void manejar_sigint(int signo) {
    (void)signo;
    sigint_recibido = 1;
}

int cancelar_entrada(void) {
    sigint_recibido = 0;
    printf("\n");
    fflush(stdout);
    return -1;
}

void restaurar_terminal(void) {
    if (terminal_activo) {
        tcsetattr(STDIN_FILENO, TCSANOW, &terminal_original);
        terminal_activo = 0;
    }
}

void instalar_manejador_sigint(void) {
    struct sigaction accion;
    memset(&accion, 0, sizeof(accion));
    accion.sa_handler = manejar_sigint;
    sigemptyset(&accion.sa_mask);

    if (sigaction(SIGINT, &accion, NULL) == -1) {
        perror("sigaction");
    }
}

void inicializar_rutas(void) {
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

    int conf = snprintf(ruta_conf, sizeof(ruta_conf), "%s/%s", directorio_datos, NOMBRE_CONF);
    int aliases_ruta = snprintf(ruta_aliases, sizeof(ruta_aliases), "%s/%s", directorio_datos, NOMBRE_ALIASES);
    int history = snprintf(ruta_historial, sizeof(ruta_historial), "%s/%s", directorio_datos, NOMBRE_HISTORIAL);

    if (conf < 0 || aliases_ruta < 0 || history < 0 ||
        (size_t)conf >= sizeof(ruta_conf) ||
        (size_t)aliases_ruta >= sizeof(ruta_aliases) ||
        (size_t)history >= sizeof(ruta_historial)) {
        fprintf(stderr, "Aviso: una ruta de datos es demasiado larga; no habrá persistencia.\n");
        return;
    }

    persistencia_disponible = 1;
}

void guardar_conf(void) {
    if (!persistencia_disponible) return;

    FILE *f = fopen(ruta_conf, "w");
    if (!f) {
        perror("No se pudo guardar la configuración");
        return;
    }

    fprintf(f, "nombre=%s\n", prompt_nombre);
    fprintf(f, "host=%s\n", prompt_host);
    fprintf(f, "simbolo=%s\n", prompt_simbolo);

    if (fclose(f) == EOF) {
        perror("No se pudo cerrar la configuración");
    }
}

int cargar_conf(void) {
    if (!persistencia_disponible) return 0;

    FILE *f = fopen(ruta_conf, "r");
    if (!f) return 0;

    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        quitar_salto(linea);
        if (strncmp(linea, "nombre=", 7) == 0) {
            copiar_texto(prompt_nombre, sizeof(prompt_nombre), linea + 7);
        } else if (strncmp(linea, "host=", 5) == 0) {
            copiar_texto(prompt_host, sizeof(prompt_host), linea + 5);
        } else if (strncmp(linea, "simbolo=", 8) == 0) {
            copiar_texto(prompt_simbolo, sizeof(prompt_simbolo), linea + 8);
        }
    }

    fclose(f);
    return 1;
}

void guardar_aliases(void) {
    if (!persistencia_disponible) return;

    FILE *f = fopen(ruta_aliases, "w");
    if (!f) {
        perror("No se pudieron guardar los aliases");
        return;
    }

    for (int i = 0; i < num_aliases; i++) {
        fprintf(f, "%s %s\n", aliases[i].nombre, aliases[i].comando);
    }

    if (fclose(f) == EOF) {
        perror("No se pudieron cerrar los aliases");
    }
}

void cargar_aliases(void) {
    if (!persistencia_disponible) return;

    FILE *f = fopen(ruta_aliases, "r");
    if (!f) return;

    char linea[512];
    while (fgets(linea, sizeof(linea), f) && num_aliases < MAX_ALIAS) {
        char nombre[32];
        char comando[256];
        quitar_salto(linea);

        if (sscanf(linea, "%31s %255[^\n]", nombre, comando) == 2) {
            copiar_texto(aliases[num_aliases].nombre, sizeof(aliases[num_aliases].nombre), nombre);
            copiar_texto(aliases[num_aliases].comando, sizeof(aliases[num_aliases].comando), comando);
            num_aliases++;
        }
    }

    fclose(f);
}

int buscar_alias(const char *nombre) {
    for (int i = 0; i < num_aliases; i++) {
        if (strcmp(aliases[i].nombre, nombre) == 0) return i;
    }
    return -1;
}

void guardar_historial(void) {
    if (!persistencia_disponible) return;

    FILE *f = fopen(ruta_historial, "w");
    if (!f) {
        perror("No se pudo guardar el historial");
        return;
    }

    for (int i = 0; i < num_historial; i++) {
        fprintf(f, "%s\n", historial[i]);
    }

    if (fclose(f) == EOF) {
        perror("No se pudo cerrar el historial");
    }
}

void agregar_historial(const char *linea) {
    if (linea[0] == '\0') return;

    if (num_historial > 0 && strcmp(historial[num_historial - 1], linea) == 0) {
        return;
    }

    if (num_historial == MAX_HISTORIAL) {
        memmove(historial, historial + 1, sizeof(historial[0]) * (MAX_HISTORIAL - 1));
        num_historial--;
    }

    copiar_texto(historial[num_historial], sizeof(historial[num_historial]), linea);
    num_historial++;
}

void cargar_historial(void) {
    if (!persistencia_disponible) return;

    FILE *f = fopen(ruta_historial, "r");
    if (!f) return;

    char linea[MAX_LINEA];
    while (fgets(linea, sizeof(linea), f)) {
        quitar_salto(linea);
        agregar_historial(linea);
    }

    fclose(f);
}

int leer_linea_basica(char *linea, size_t tam_linea) {
    errno = 0;
    if (fgets(linea, tam_linea, stdin)) {
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

int pedir_linea_basica(const char *mensaje, char *linea, size_t tam_linea) {
    while (1) {
        printf("%s", mensaje);
        fflush(stdout);

        int resultado = leer_linea_basica(linea, tam_linea);
        if (resultado != -1) return resultado;
    }
}

void configurar_prompt(void) {
    if (cargar_conf()) return;

    printf("=== QUANTIC SHELL - CONFIGURACION INICIAL ===\n");

    if (pedir_linea_basica("Nombre (enter = usuario): ", prompt_nombre, sizeof(prompt_nombre)) == 0) return;
    if (prompt_nombre[0] == '\0') {
        copiar_texto(prompt_nombre, sizeof(prompt_nombre), getenv("USER") ? getenv("USER") : "usuario");
    }

    if (pedir_linea_basica("Host (enter = hostname): ", prompt_host, sizeof(prompt_host)) == 0) return;
    if (prompt_host[0] == '\0') {
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) == -1) {
            copiar_texto(hostname, sizeof(hostname), "localhost");
        }
        hostname[sizeof(prompt_host) - 1] = '\0';
        copiar_texto(prompt_host, sizeof(prompt_host), hostname);
    }

    if (pedir_linea_basica("Simbolo (enter = $): ", prompt_simbolo, sizeof(prompt_simbolo)) == 0) return;
    if (prompt_simbolo[0] == '\0') {
        copiar_texto(prompt_simbolo, sizeof(prompt_simbolo), "$");
    }

    guardar_conf();
    if (persistencia_disponible) {
        printf("\nConfiguracion guardada en %s\n\n", ruta_conf);
    }
}

void mostrar_prompt(void) {
    char ruta[MAX_RUTA];
    if (!getcwd(ruta, sizeof(ruta))) {
        copiar_texto(ruta, sizeof(ruta), "?");
    }

    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        size_t home_len = strlen(home);
        if (strncmp(home, ruta, home_len) == 0 &&
            (ruta[home_len] == '/' || ruta[home_len] == '\0')) {
            memmove(ruta + 1, ruta + home_len, strlen(ruta + home_len) + 1);
            ruta[0] = '~';
        }
    }

    printf("\x1b[32m%s\x1b[0m@\x1b[34m%s\x1b[0m \x1b[35m%s\x1b[0m \x1b[36m%s\x1b[0m ",
           prompt_nombre, prompt_host, ruta, prompt_simbolo);
    fflush(stdout);
}

void redibujar_linea(const char *linea) {
    printf("\r\x1b[2K");
    mostrar_prompt();
    fputs(linea, stdout);
    fflush(stdout);
}

int activar_modo_edicion(void) {
    if (tcgetattr(STDIN_FILENO, &terminal_original) == -1) return 0;

    struct termios modo = terminal_original;
    modo.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    modo.c_cc[VMIN] = 1;
    modo.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &modo) == -1) return 0;

    terminal_activo = 1;
    return 1;
}

int leer_caracter_terminal(char *caracter) {
    ssize_t leidos = read(STDIN_FILENO, caracter, 1);
    if (leidos == 1) return 1;
    if (leidos == 0) return 0;
    if (errno == EINTR) return -1;
    return -2;
}

int finalizar_lectura_terminal(int resultado) {
    restaurar_terminal();

    if (resultado == -1 && sigint_recibido) return cancelar_entrada();

    if (resultado == -2) perror("read");
    return 0;
}

void cargar_entrada_historial(char *linea, size_t tam_linea, int indice) {
    copiar_texto(linea, tam_linea, historial[indice]);
    redibujar_linea(linea);
}

int leer_linea_interactiva(char *linea, size_t tam_linea) {
    if (!isatty(STDIN_FILENO) || !activar_modo_edicion()) {
        return leer_linea_basica(linea, tam_linea);
    }

    linea[0] = '\0';
    char borrador[MAX_LINEA] = "";
    size_t longitud = 0;
    int indice_historial = num_historial;

    while (1) {
        char caracter;
        int resultado = leer_caracter_terminal(&caracter);
        if (resultado != 1) return finalizar_lectura_terminal(resultado);

        if (caracter == '\r' || caracter == '\n') {
            restaurar_terminal();
            printf("\n");
            fflush(stdout);
            return 1;
        }

        if (caracter == 4 && longitud == 0) {
            restaurar_terminal();
            return 0;
        }

        if (caracter == 127 || caracter == '\b') {
            if (longitud > 0) {
                linea[--longitud] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (caracter == '\x1b') {
            char segundo;
            char tercero;
            int primero = leer_caracter_terminal(&segundo);
            if (primero != 1) return finalizar_lectura_terminal(primero);
            int segundo_resultado = leer_caracter_terminal(&tercero);
            if (segundo_resultado != 1) return finalizar_lectura_terminal(segundo_resultado);

            if (segundo == '[' && tercero == 'A' && num_historial > 0) {
                if (indice_historial == num_historial) {
                    copiar_texto(borrador, sizeof(borrador), linea);
                }
                if (indice_historial > 0) indice_historial--;
                cargar_entrada_historial(linea, tam_linea, indice_historial);
                longitud = strlen(linea);
            } else if (segundo == '[' && tercero == 'B' && indice_historial < num_historial) {
                indice_historial++;
                if (indice_historial == num_historial) {
                    copiar_texto(linea, tam_linea, borrador);
                    redibujar_linea(linea);
                } else {
                    cargar_entrada_historial(linea, tam_linea, indice_historial);
                }
                longitud = strlen(linea);
            }
            continue;
        }

        if (isprint((unsigned char)caracter) && longitud + 1 < tam_linea) {
            linea[longitud++] = caracter;
            linea[longitud] = '\0';
            fputc(caracter, stdout);
            fflush(stdout);
        }
    }
}

void cmd_exit(char *args) {
    (void)args;
    exit(0);
}

void cmd_cd(char *args) {
    if (args) {
        if (chdir(args) != 0) {
            printf("cd: no existe '%s'\n", args);
        }
    } else {
        const char *home = getenv("HOME");
        if (home && chdir(home) != 0) {
            printf("cd: no se pudo ir a HOME\n");
        }
    }
}

void cmd_help(char *args) {
    (void)args;
    printf("Comandos:\n");
    printf("  exit                         - salir\n");
    printf("  cd [ruta]                    - cambiar de directorio\n");
    printf("  help                         - esta ayuda\n");
    printf("  alias [nombre comando]       - crear, actualizar o listar aliases\n");
    printf("  config                       - editar el prompt\n");
    printf("  comando1 ; comando2          - ejecutar comandos en cadena\n");
    printf("  flecha arriba/abajo          - navegar por el historial\n");
    printf("  Ctrl+C                       - cancelar la entrada o el comando actual\n");
    printf("  Los demás comandos se ejecutan mediante /bin/sh.\n");
}

void cmd_alias(char *args) {
    if (!args) {
        if (num_aliases == 0) {
            printf("No hay aliases.\n");
            return;
        }

        printf("Aliases:\n");
        for (int i = 0; i < num_aliases; i++) {
            printf("  %-12s -> %s\n", aliases[i].nombre, aliases[i].comando);
        }
        return;
    }

    char nombre[32] = "";
    char comando[256] = "";
    if (sscanf(args, "%31s %255[^\n]", nombre, comando) != 2) {
        printf("Uso: alias <nombre> <comando>\n");
        return;
    }

    int indice = buscar_alias(nombre);
    if (indice != -1) {
        copiar_texto(aliases[indice].comando, sizeof(aliases[indice].comando), comando);
        guardar_aliases();
        printf("Alias '%s' actualizado.\n", nombre);
        return;
    }

    if (num_aliases == MAX_ALIAS) {
        printf("Error: limite de aliases alcanzado.\n");
        return;
    }

    copiar_texto(aliases[num_aliases].nombre, sizeof(aliases[num_aliases].nombre), nombre);
    copiar_texto(aliases[num_aliases].comando, sizeof(aliases[num_aliases].comando), comando);
    num_aliases++;
    guardar_aliases();
    printf("Alias '%s' creado.\n", nombre);
}

int pedir_valor_config(const char *mensaje, char *destino, size_t tam_destino) {
    char temporal[MAX_LINEA];
    int resultado = pedir_linea_basica(mensaje, temporal, sizeof(temporal));
    if (resultado == 1 && temporal[0] != '\0') {
        copiar_texto(destino, tam_destino, temporal);
    }
    return resultado;
}

void cmd_config(char *args) {
    (void)args;
    printf("=== EDITAR PROMPT ===\n");
    printf("nombre=%s\n", prompt_nombre);
    printf("host=%s\n", prompt_host);
    printf("simbolo=%s\n\n", prompt_simbolo);

    if (pedir_valor_config("Nuevo nombre (enter = mantener): ", prompt_nombre, sizeof(prompt_nombre)) == 0) return;
    if (pedir_valor_config("Nuevo host (enter = mantener): ", prompt_host, sizeof(prompt_host)) == 0) return;
    if (pedir_valor_config("Nuevo simbolo (enter = mantener): ", prompt_simbolo, sizeof(prompt_simbolo)) == 0) return;

    guardar_conf();
    if (persistencia_disponible) printf("Guardado.\n");
}

Comando comandos[] = {
    {"exit", cmd_exit},
    {"cd", cmd_cd},
    {"help", cmd_help},
    {"alias", cmd_alias},
    {"config", cmd_config},
    {NULL, NULL}
};

void ejecutar_externo(const char *texto) {
    pid_t hijo = fork();
    if (hijo == -1) {
        perror("fork");
        return;
    }

    if (hijo == 0) {
        struct sigaction accion;
        memset(&accion, 0, sizeof(accion));
        accion.sa_handler = SIG_DFL;
        sigemptyset(&accion.sa_mask);
        sigaction(SIGINT, &accion, NULL);
        execl("/bin/sh", "sh", "-c", texto, (char *)NULL);
        perror("/bin/sh");
        _exit(127);
    }

    int estado;
    while (waitpid(hijo, &estado, 0) == -1) {
        if (errno != EINTR) {
            perror("waitpid");
            break;
        }
    }
    sigint_recibido = 0;
}

char *separar_comando(char *texto, char **args) {
    char *cursor = texto;
    while (*cursor && !isspace((unsigned char)*cursor)) cursor++;

    if (*cursor) {
        *cursor++ = '\0';
        cursor = recortar(cursor);
    }

    *args = *cursor ? cursor : NULL;
    return texto;
}

void despachar_segmento(char *segmento) {
    char *texto = recortar(segmento);
    if (texto[0] == '\0') return;

    char copia[MAX_LINEA];
    copiar_texto(copia, sizeof(copia), texto);

    char *args;
    char *comando = separar_comando(copia, &args);
    for (int i = 0; comandos[i].nombre != NULL; i++) {
        if (strcmp(comando, comandos[i].nombre) == 0) {
            comandos[i].funcion(args);
            return;
        }
    }

    int indice_alias = buscar_alias(comando);
    if (indice_alias != -1) {
        char final[MAX_LINEA + 256];
        int escritos;
        if (args) {
            escritos = snprintf(final, sizeof(final), "%s %s", aliases[indice_alias].comando, args);
        } else {
            escritos = snprintf(final, sizeof(final), "%s", aliases[indice_alias].comando);
        }

        if (escritos < 0 || (size_t)escritos >= sizeof(final)) {
            printf("Alias demasiado largo.\n");
            return;
        }

        ejecutar_externo(final);
        return;
    }

    ejecutar_externo(texto);
}

void ejecutar_linea(char *linea) {
    char comilla = '\0';
    int escapado = 0;
    char *inicio = linea;

    for (char *cursor = linea; ; cursor++) {
        char caracter = *cursor;

        if (caracter == '\0') {
            despachar_segmento(inicio);
            return;
        }

        if (escapado) {
            escapado = 0;
            continue;
        }

        if (caracter == '\\' && comilla != '\'') {
            escapado = 1;
            continue;
        }

        if (comilla != '\0') {
            if (caracter == comilla) comilla = '\0';
            continue;
        }

        if (caracter == '\'' || caracter == '"') {
            comilla = caracter;
        } else if (caracter == ';') {
            *cursor = '\0';
            despachar_segmento(inicio);
            inicio = cursor + 1;
        }
    }
}

int main(void) {
    char linea[MAX_LINEA];

    inicializar_rutas();
    instalar_manejador_sigint();
    atexit(restaurar_terminal);

    configurar_prompt();
    cargar_aliases();
    cargar_historial();

    printf("\x1b[1m=== QUANTIC SHELL v1.0 ===\x1b[0m\n");

    while (1) {
        mostrar_prompt();

        int resultado = leer_linea_interactiva(linea, sizeof(linea));
        if (resultado == 0) break;
        if (resultado == -1) continue;
        if (linea[0] == '\0') continue;

        agregar_historial(linea);
        guardar_historial();
        ejecutar_linea(linea);
    }

    printf("\nAdios.\n");
    return 0;
}
