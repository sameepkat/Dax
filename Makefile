VERSION ?= development
DAX_INCLUDES = -Ivendor/janet -Ibuild/include -Ibuild \
	-Isrc/app -Isrc/shell -Isrc/syntax -Isrc/execution \
	-Isrc/builtins -Isrc/interactive -Isrc/janet -Isrc/support
CXXFLAGS += -Wall -Wextra -std=c++17

SRC = $(shell find src -type f -name '*.cpp' -print | sort)
OBJ = $(patsubst %.cpp, build/%.o, $(SRC))
DEP = $(OBJ:.o=.d)
JANET_OBJ = build/janet.o
TARGET = dax

# Installation directories
PREFIX ?= /usr
BINDIR = $(PREFIX)/bin

# Create build/ directory if it doesn't exist
$(shell mkdir -p build)

# Declare the target and its dependencies
$(TARGET): $(OBJ) $(JANET_OBJ) build/readline/libreadline.a build/link-libs
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $(TARGET) $(OBJ) $(JANET_OBJ) build/readline/libreadline.a $$(sed -n 's/^TERMCAP_LIB = //p' build/readline/Makefile) $$(cat build/link-libs) $(LDLIBS)

build/link-libs: build-aux/link-libs.sh
	@mkdir -p build
	@CC='$(CC)' CFLAGS='$(CFLAGS)' LDFLAGS='$(LDFLAGS)' sh $< > $@.tmp && mv $@.tmp $@

build/readline/Makefile: vendor/readline/configure
	@mkdir -p build/readline build/include
	ln -sfn ../../vendor/readline build/include/readline
	cd build/readline && CC='$(CC)' CFLAGS='$(CFLAGS)' CPPFLAGS='$(CPPFLAGS)' LDFLAGS='$(LDFLAGS)' ../../vendor/readline/configure --disable-shared --with-curses

build/readline/libreadline.a: build/readline/Makefile
	$(MAKE) -C build/readline static

$(OBJ): | build/readline/Makefile

build/version.hpp: FORCE
	@mkdir -p build
	@printf '#pragma once\n#define DAX_VERSION "%s"\n' "$(VERSION)" > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi

build/src/app/main.o: build/version.hpp
build/src/builtins/builtins.o: build/version.hpp

# Rule for generating object files from source files
build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(DAX_INCLUDES) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/janet.o: vendor/janet/janet.c vendor/janet/janet.h
	$(CC) $(DAX_INCLUDES) $(CPPFLAGS) $(CFLAGS) -w -std=c99 -fPIC -c vendor/janet/janet.c -o $@

# Install target
build/pattern-match-test: tests/unit/pattern_match.cpp src/support/pattern_match.cpp src/support/pattern_match.hpp
	@mkdir -p build
	$(CXX) $(DAX_INCLUDES) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/unit/pattern_match.cpp src/support/pattern_match.cpp $(LDLIBS)

test: $(TARGET) build/pattern-match-test
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	  ./build/pattern-match-test && \
	  POSIXLY_CORRECT=1 ./build/pattern-match-test --posix && \
	  HOME="$$tmp" XDG_CONFIG_HOME="$$tmp/config" python3 tests/shell/pattern_matching.py ./$(TARGET) && \
	  LC_ALL=C HOME="$$tmp" XDG_CONFIG_HOME="$$tmp/config" ./$(TARGET) tests/shell/smoke.dax && \
	  HOME="$$tmp" XDG_CONFIG_HOME="$$tmp/config" python3 tests/interactive/history_suggestion.py ./$(TARGET)

install: $(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

# Uninstall target
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJ) $(DEP) $(JANET_OBJ)
	rm -rf build/readline build/include/readline
	rm -f build/link-libs build/link-libs.tmp
	rm -f build/pattern-match-test

-include $(DEP)

.PHONY: clean install uninstall test FORCE
FORCE:
