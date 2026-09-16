#!/bin/bash

# Quantic Shell - Script de instalación automática
# Detecta la arquitectura y compila el binario automáticamente

set -e

echo "=== Quantic Shell - Instalador ==="
echo ""

# Detectar sistema operativo
OS="$(uname -s)"
case "$OS" in
    Linux*)     OS_NAME="Linux";;
    Darwin*)    OS_NAME="macOS";;
    *)          echo "Error: Sistema operativo no soportado: $OS"; exit 1;;
esac

echo "Sistema operativo: $OS_NAME"

# Detectar arquitectura
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)     ARCH_NAME="x86_64";;
    aarch64)    ARCH_NAME="ARM64";;
    arm64)      ARCH_NAME="ARM64";;
    armv7l)     ARCH_NAME="ARMv7";;
    i686)       ARCH_NAME="x86";;
    *)          echo "Error: Arquitectura no soportada: $ARCH"; exit 1;;
esac

echo "Arquitectura: $ARCH_NAME"
echo ""

# Verificar que existe el código fuente
if [ ! -f "quanticshell.c" ]; then
    echo "Error: No se encuentra quanticshell.c en el directorio actual"
    echo "Por favor, ejecuta este script desde el directorio del proyecto"
    exit 1
fi

# Verificar que existe un compilador C
if ! command -v cc &> /dev/null && ! command -v gcc &> /dev/null; then
    echo "Error: No se encontró un compilador C (cc o gcc)"
    echo "Por favor, instala un compilador C primero:"
    echo "  Ubuntu/Debian: sudo apt install build-essential"
    echo "  macOS: xcode-select --install"
    echo "  Fedora: sudo dnf install gcc"
    exit 1
fi

# Determinar qué compilador usar
if command -v cc &> /dev/null; then
    CC="cc"
else
    CC="gcc"
fi

echo "Compilador: $CC"
echo ""

# Compilar el binario
echo "Compilando quanticshell..."
$CC -std=c11 -Wall -Wextra -Wpedantic -O2 -o quanticshell quanticshell.c

if [ $? -ne 0 ]; then
    echo "Error: La compilación falló"
    exit 1
fi

echo "✓ Compilación exitosa"
echo ""

# Hacer el binario ejecutable
chmod +x quanticshell

# Crear directorio ~/.local/bin si no existe
if [ ! -d "$HOME/.local/bin" ]; then
    echo "Creando ~/.local/bin..."
    mkdir -p "$HOME/.local/bin"
fi

# Crear symlink en ~/.local/bin
echo "Creando symlink en ~/.local/bin/quanticshell..."
ln -sf "$(pwd)/quanticshell" "$HOME/.local/bin/quanticshell"

# Verificar que ~/.local/bin está en PATH
if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
    echo ""
    echo "⚠️  Advertencia: ~/.local/bin no está en tu PATH"
    echo "Añade esta línea a tu ~/.bashrc o ~/.zshrc:"
    echo '  export PATH="$HOME/.local/bin:$PATH"'
    echo ""
    echo "Luego ejecuta: source ~/.bashrc"
fi

echo ""
echo "=== Instalación completada ==="
echo ""
echo "Puedes ejecutar quanticshell de dos formas:"
echo "  1. Desde este directorio: ./quanticshell"
echo "  2. Desde cualquier lugar: quanticshell"
echo ""
echo "Para empezar:"
echo "  quanticshell"
echo ""
