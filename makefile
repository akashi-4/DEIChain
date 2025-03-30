CC = gcc
CFLAGS = -Wall -pthread
LDFLAGS = -pthread

# Directories
SRC_DIR = src
BIN_DIR = bin

# Create bin directory if it doesn't exist
$(shell mkdir -p $(BIN_DIR))

# Common source files
COMMON_SOURCES = $(SRC_DIR)/logger.c

# Controller-specific sources
CONTROLLER_SOURCES = $(SRC_DIR)/controller.c $(COMMON_SOURCES)
CONTROLLER_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(CONTROLLER_SOURCES))
CONTROLLER_EXECUTABLE = controller

# TxGen-specific sources
TXGEN_SOURCES = $(SRC_DIR)/txgen.c $(COMMON_SOURCES)
TXGEN_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(TXGEN_SOURCES))
TXGEN_EXECUTABLE = txgen

# Default target builds both executables
all: $(CONTROLLER_EXECUTABLE) $(TXGEN_EXECUTABLE)

# Build controller
$(CONTROLLER_EXECUTABLE): $(CONTROLLER_OBJECTS)
	$(CC) $(LDFLAGS) $(CONTROLLER_OBJECTS) -o $@

# Build txgen
$(TXGEN_EXECUTABLE): $(TXGEN_OBJECTS)
	$(CC) $(LDFLAGS) $(TXGEN_OBJECTS) -o $@

# Generic rule for object files
$(BIN_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean everything
clean:
	rm -rf $(BIN_DIR) $(CONTROLLER_EXECUTABLE) $(TXGEN_EXECUTABLE) *.a *.so *.o *~ core

# Run both programs (for convenience)
run: all
	./$(CONTROLLER_EXECUTABLE) & ./$(TXGEN_EXECUTABLE) 2 1000
