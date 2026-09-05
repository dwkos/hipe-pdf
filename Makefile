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

.PHONY: all clean third-party
