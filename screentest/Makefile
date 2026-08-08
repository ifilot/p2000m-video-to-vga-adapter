# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

ASSEMBLER ?= z80asm
TARGET := screentest.bin
SOURCE := screentest.asm

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(ASSEMBLER) $(SOURCE) -o $(TARGET)

clean:
	$(RM) $(TARGET)
