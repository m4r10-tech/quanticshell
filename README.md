# Quantic Shell

Una shell minimalista escrita en C diseñada como proyecto educativo para aprender programación de sistemas POSIX. Implementa las funcionalidades esenciales de una shell interactiva moderna mientras practica conceptos fundamentales de C.

## Características

### Funcionalidades implementadas

- **Prompt configurable**: Personaliza nombre de usuario, host y símbolo del prompt
- **Directorio actual**: Muestra la ruta actual con acortamiento automático de `~` para el directorio home
- **Colores ANSI**: Interfaz visual con colores para distinguir diferentes partes del prompt
- **Comandos internos**: `cd`, `help`, `exit`, `config`, `alias`
- **Aliases persistentes**: Crea atajos que sobreviven entre sesiones
- **Encadenamiento de comandos**: Ejecuta múltiples comandos con `;`
- **Historial navegable**: Usa las flechas ↑/↓ para recorrer comandos anteriores
- **Historial persistente**: El historial se guarda y carga automáticamente
- **Manejo de señales**: Ctrl+C cancela comandos sin cerrar la shell
- **Ejecución de comandos externos**: Soporta pipes, redirecciones y globbing vía `/bin/sh`
- **Modo interactivo y no interactivo**: Funciona tanto en terminal como con pipes

## Instalación

### Requisitos

- Compilador C compatible con C11 (GCC, Clang)
- Sistema operativo Unix/Linux/macOS
- make (opcional, para usar el Makefile)

### Compilación manual

```bash
# Clonar el repositorio
git clone https://github.com/tu-usuario/quanticshell.git
cd quanticshell

# Compilar
cc -std=c11 -Wall -Wextra -Wpedantic -o quanticshell quanticshell.c

# Ejecutar
./quanticshell
```

### Instalación global (opcional)

Para tener `quanticshell` disponible en cualquier directorio:

```bash
# Compilar
cc -std=c11 -Wall -Wextra -Wpedantic -o quanticshell quanticshell.c

# Copiar a ~/.local/bin (asegúrate de que esté en tu PATH)
cp quanticshell ~/.local/bin/

# Ahora puedes ejecutar desde cualquier lugar
quanticshell
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

### Comandos externos

Cualquier comando que no sea interno se ejecuta mediante `/bin/sh -c`, lo que permite:

```bash
# Pipes
ls -la | grep ".c$" | wc -l

# Redirecciones
echo "hola" > archivo.txt
cat archivo.txt >> otro.txt

# Globbing
ls *.c
rm *.o

# Sustitución de comandos
echo "Hoy es $(date)"

# Variables de entorno
export MI_VAR="valor"
echo $MI_VAR
```

### Historial

- **↑ (flecha arriba)**: Comando anterior
- **↓ (flecha abajo)**: Comando siguiente
- **Ctrl+C**: Cancela el comando actual o la línea en edición
- **Ctrl+D**: Sale de la shell (si la línea está vacía)

El historial guarda las últimas 100 líneas en `~/.quanticshell/history`.

## Estructura del proyecto

```
quanticshell/
├── quanticshell.c      # Código fuente completo de la shell
├── README.md           # Esta documentación
├── .gitignore          # Archivos ignorados por Git
└── LICENSE             # Licencia MIT

Archivos generados en runtime (en ~/.quanticshell/):
├── config              # Configuración del prompt
├── aliases             # Aliases persistentes
└── history             # Historial de comandos
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

### 4. Procesamiento de comandos
- `ejecutar_linea()`: Parsea y ejecuta líneas con `;`
- `despachar_segmento()`: Determina si es builtin, alias o externo
- `ejecutar_externo()`: Ejecuta comandos externos con fork/exec

### 5. Manejo de señales
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

## Limitaciones actuales

Esta es una shell educativa con limitaciones intencionales:

- **Sin control de trabajos**: No soporta `jobs`, `fg`, `bg` nativamente
- **Sin autocompletado**: No hay tab completion
- **Parser simple**: El parser de `;` no maneja todas las construcciones de shell
- **Sin variables de shell**: No mantiene variables internas (usa las del entorno)
- **Sin scripts**: No puede ejecutar scripts de la shell
- **Sin job control avanzado**: No soporta suspensión con Ctrl+Z

Estas limitaciones son intencionales para mantener el código simple y educativo.

## Roadmap

Posibles mejoras futuras:

- [ ] Control de trabajos (jobs, fg, bg)
- [ ] Autocompletado con Tab
- [ ] Variables de shell internas
- [ ] Ejecución de scripts
- [ ] Colores configurables
- [ ] Temas de prompt
- [ ] Soporte para múltiples sesiones
- [ ] Sincronización de historial entre sesiones
- [ ] Makefile para facilitar la compilación
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
- Compila sin warnings con `-Wall -Wextra -Wpedantic`
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

**Nota**: Este proyecto es principalmente educativo. Para uso diario, se recomienda usar shells más completas como bash, zsh o fish.
