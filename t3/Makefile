# Makefile Simulador de Gerência de Memória
# INF01142 Sistemas Operacionais 2026/1

CC     = gcc
# -Wno-unused-function: stubs ainda não implementados geram este aviso
CFLAGS = -Werror -Wall -Wextra -std=c11 -Wno-unused-function

TARGET = memsim

all: $(TARGET)

$(TARGET): memsim.c
	$(CC) $(CFLAGS) -o $@ $<

run-contiguous: all
	./$(TARGET) contiguous

run-paged: all
	./$(TARGET) paged

run-validate: all
	./$(TARGET) validate

clean:
	rm -f $(TARGET)
