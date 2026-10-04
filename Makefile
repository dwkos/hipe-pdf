# Copyright (c) 2026 Daniel Kos
#
# This file is part of hipe-pdf, licensed under the GNU General Public License
# version 3 or later. See COPYING and LICENSE.md.

CXX?=g++

MUPDF_DIR=third_party/mupdf
MUPDF_BUILD=$(MUPDF_DIR)/build/release

CXXFLAGS=-Wall -O2 -std=c++17 -I$(MUPDF_DIR)/include
LDFLAGS=-L$(MUPDF_BUILD) -lmupdf -lmupdf-third -lhipe -lm -lpthread

SRC=src/main.cpp src/pdf_document.cpp
OBJ=$(SRC:src/%.cpp=build/%.o)
BIN=build/hipe-pdf

all: $(BIN)

build:
	mkdir -p build

build/%.o: src/%.cpp src/pdf_document.hpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CXX) $(OBJ) -o $(BIN) $(LDFLAGS)

third-party:
	cd $(MUPDF_DIR) && $(MAKE) build=release libs

clean:
	rm -f build/*.o $(BIN)

# Wipe the vendored MuPDF's build output (objects + libmupdf*.a) so `make
# third-party` rebuilds it from scratch -- e.g. after moving the tree to a
# different machine/toolchain. Leaves the checked-out MuPDF source untouched.
clean-third-party:
	rm -rf $(MUPDF_DIR)/build

distclean: clean clean-third-party

# The complete source of a binary built from the current commit, MuPDF included, to publish
# next to it -- see scripts/source-bundle.sh and LICENSE.md.
source-bundle:
	scripts/source-bundle.sh build

.PHONY: all clean clean-third-party distclean third-party source-bundle
