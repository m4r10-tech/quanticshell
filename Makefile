# Quantic Shell - Makefile
# Facilita la compilación del proyecto

CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
TARGET = quanticshell
SRC = quanticshell.c

.PHONY: all clean install test bench help

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)
	@echo "✓ Compilación completada: $(TARGET)"

clean:
	rm -f $(TARGET)
	@echo "✓ Binario eliminado"

install: $(TARGET)
	@chmod +x $(TARGET)
	@mkdir -p $(HOME)/.local/bin
	@ln -sf $(PWD)/$(TARGET) $(HOME)/.local/bin/$(TARGET)
	@echo "✓ Instalación completada"
	@echo "✓ Symlink creado en ~/.local/bin/quanticshell"
	@echo ""
	@echo "Puedes ejecutar:"
	@echo "  ./quanticshell    (desde este directorio)"
	@echo "  quanticshell      (desde cualquier lugar)"

test: $(TARGET)
	@bash tests/run.sh

bench: $(TARGET)
	@bash tests/bench.sh

help:
	@echo "Quantic Shell - Comandos disponibles:"
	@echo "  make          - Compilar el binario"
	@echo "  make clean    - Eliminar el binario compilado"
	@echo "  make install  - Compilar e instalar en ~/.local/bin"
	@echo "  make test     - Compilar y correr la suite de tests"
	@echo "  make bench    - Benchmarks vs bash/dash"
	@echo "  make help     - Mostrar esta ayuda"
