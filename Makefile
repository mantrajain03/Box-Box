# ===========================================================================
#  Box Box — F1 Pit-Stop & Race Strategy Scheduler
#
#  The single build entry point for the project (NFR-6). There are no helper
#  scripts in any language; everything this repository builds is a C++ program.
#
#  Default target builds the core library objects plus the CLI and depends on
#  nothing outside the C++17 standard library (NFR-1).
# ===========================================================================

CXX      ?= g++
CXXSTD   := -std=c++17
WARN     := -Wall -Wextra
OPT      ?= -O2

# Assigned with ':=' rather than '?=' so that -std/-Wall/-Wextra always apply
# even if the environment already exports CXXFLAGS. Use EXTRA_* to add flags.
CPPFLAGS := -Iinclude $(EXTRA_CPPFLAGS)
CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(EXTRA_CXXFLAGS)
LDFLAGS  := $(EXTRA_LDFLAGS)

# ---------------------------------------------------------------------------
# Host detection
#
# Two things here are deliberately not done the obvious way:
#
#  1. Shell. GNU Make on Windows silently falls back to cmd.exe when it cannot
#     find a POSIX sh on PATH, yet $(SHELL) reports "sh.exe" in BOTH cases, so
#     $(SHELL) cannot tell them apart. Probe behaviour instead: a POSIX shell
#     strips the quotes from `echo "probe"`, cmd.exe prints them literally.
#
#  2. Executable suffix. $(OS) is *unset* under MSYS2 bash even though the
#     target is still Windows, so ask the compiler what it actually targets.
# ---------------------------------------------------------------------------
ifeq ($(shell echo "probe"),probe)
    POSIX_SHELL := 1
else
    POSIX_SHELL :=
endif

TARGET_TRIPLE := $(shell $(CXX) -dumpmachine)
WINDOWS_TARGET := $(findstring mingw,$(TARGET_TRIPLE))$(findstring cygwin,$(TARGET_TRIPLE))$(findstring msys,$(TARGET_TRIPLE))

ifneq ($(WINDOWS_TARGET),)
    EXE := .exe
else
    EXE :=
endif

ifeq ($(POSIX_SHELL),1)
    mkdir_p  = mkdir -p "$(1)"
    rmdir_r  = rm -rf "$(1)"
    exe_path = ./$(1)
else
    mkdir_p  = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
    rmdir_r  = if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
    exe_path = $(subst /,\,$(1))
endif

# Expands to the mkdir incantation for the directory holding the current target.
MKDIR_FOR_TARGET = $(call mkdir_p,$(patsubst %/,%,$(dir $@)))

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------
BUILD_DIR := build
BIN_DIR   := bin

CORE_SRC  := $(wildcard src/*.cpp)
CLI_SRC   := $(wildcard cli/*.cpp)
TEST_SRC  := $(wildcard tests/*.cpp)

CORE_OBJ  := $(CORE_SRC:%.cpp=$(BUILD_DIR)/%.o)
CLI_OBJ   := $(CLI_SRC:%.cpp=$(BUILD_DIR)/%.o)
TEST_OBJ  := $(TEST_SRC:%.cpp=$(BUILD_DIR)/%.o)

CLI_BIN   := $(BIN_DIR)/boxbox$(EXE)
TEST_BIN  := $(BIN_DIR)/boxbox_tests$(EXE)

DEPS      := $(CORE_OBJ:.o=.d) $(CLI_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all
all: $(CLI_BIN)

$(CLI_BIN): $(CORE_OBJ) $(CLI_OBJ)
	@$(call mkdir_p,$(BIN_DIR))
	$(CXX) $(CXXFLAGS) $(CORE_OBJ) $(CLI_OBJ) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.cpp
	@$(MKDIR_FOR_TARGET)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# ARGS lets you pass CLI options through, e.g.
#   make run ARGS="--laps 4 --pit-loss 5 --sector 10"
.PHONY: run
run: $(CLI_BIN)
	$(call exe_path,$(CLI_BIN)) $(ARGS)

# Phase 5. A hand-written assert harness linked against the core objects only:
# it does not build or need the CLI, and it must never depend on the GUI.
$(TEST_BIN): $(CORE_OBJ) $(TEST_OBJ)
	@$(call mkdir_p,$(BIN_DIR))
	$(CXX) $(CXXFLAGS) $(CORE_OBJ) $(TEST_OBJ) -o $@ $(LDFLAGS)

.PHONY: test
test: $(TEST_BIN)
	$(call exe_path,$(TEST_BIN))

.PHONY: clean
clean:
	@$(call rmdir_r,$(BUILD_DIR))
	@$(call rmdir_r,$(BIN_DIR))

# NOTE: recipe text below deliberately avoids ( ) " ' ; | & < > and bare dots.
# These recipes must run verbatim under BOTH cmd.exe and POSIX sh, and each
# shell treats a different subset of those characters as syntax.
.PHONY: info
info:
	@echo Box Box build configuration
	@echo   CXX           = $(CXX)
	@echo   target triple = $(TARGET_TRIPLE)
	@echo   exe suffix    = $(EXE)
	@echo   posix shell   = $(POSIX_SHELL)
	@echo   CXXFLAGS      = $(CXXFLAGS)
	@echo   CPPFLAGS      = $(CPPFLAGS)
	@echo   core sources  = $(CORE_SRC)
	@echo   cli sources   = $(CLI_SRC)
	@echo   cli binary    = $(CLI_BIN)

.PHONY: help
help:
	@echo Box Box - F1 Pit-Stop and Race Strategy Scheduler
	@echo   make          Build the core library and the CLI. This is the default.
	@echo   make test     Build and run the C++ assert harness
	@echo   make run      Build then run the CLI. Pass options via ARGS=...
	@echo   make clean    Remove build/ and bin/
	@echo   make info     Print the detected toolchain configuration
	@echo   make help     This message
	@echo   Not yet implemented: make gui is Phase 6.
	@echo   CLI options are listed by: make run ARGS=--help

-include $(DEPS)
