CXX = g++
VERSION ?= development
CXXFLAGS = -Wall -Wextra -std=c++17 -Ivendor/janet -Ibuild

SRC = $(wildcard *.cpp)
OBJ = $(patsubst %.cpp, build/%.o, $(SRC))  # Object files in build/ directory
DEP = $(OBJ:.o=.d)
JANET_OBJ = build/janet.o
TARGET = dax

# Installation directories
PREFIX ?= /usr
BINDIR = $(PREFIX)/bin

# Create build/ directory if it doesn't exist
$(shell mkdir -p build)

# Declare the target and its dependencies
$(TARGET): $(OBJ) $(JANET_OBJ)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJ) $(JANET_OBJ) -lreadline -lm -lpthread -lrt -ldl

build/version.hpp: FORCE
	@mkdir -p build
	@printf '#pragma once\n#define DAX_VERSION "%s"\n' "$(VERSION)" > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi

build/main.o: build/version.hpp

# Rule for generating object files from source files
build/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/janet.o: vendor/janet/janet.c vendor/janet/janet.h
	$(CC) -w -std=c99 -fPIC -Ivendor/janet -c vendor/janet/janet.c -o $@

# Install target
install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

# Uninstall target
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET) build/*.o build/*.d

-include $(DEP)

.PHONY: clean install uninstall FORCE
FORCE:
