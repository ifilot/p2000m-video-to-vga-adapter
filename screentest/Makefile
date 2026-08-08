# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

ASSEMBLER ?= z80asm
PYTHON ?= python3
TARGET := screentest.bin
UNSIGNED := screentest.unsigned.bin
SOURCE := screentest.asm
SIGNER := sign_cartridge.py

.PHONY: all clean

all: $(TARGET)

$(UNSIGNED): $(SOURCE)
	$(ASSEMBLER) $(SOURCE) -o $(UNSIGNED)

$(TARGET): $(UNSIGNED) $(SIGNER)
	$(PYTHON) $(SIGNER) $(UNSIGNED) $(TARGET)

clean:
	$(RM) $(TARGET) $(UNSIGNED)
