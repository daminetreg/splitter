CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
CXXFLAGS += -I/nix/store/hhpbly4mjks6i32sl15clxwfgnszda6x-clang-19.1.7-dev/include
CXXFLAGS += -I/nix/store/hp8fsx1sg3dadia1i092188ll9d216sg-boost-1.87.0-dev/include

LDFLAGS := -lclang
LDFLAGS += -L/nix/store/9b8h4b98vj92v7ql5chmim0447x2w220-clang-19.1.7-lib/lib
LDFLAGS += -Wl,-rpath,/nix/store/9b8h4b98vj92v7ql5chmim0447x2w220-clang-19.1.7-lib/lib
LDFLAGS += -lpthread

TARGET := cpp-splitter
SRC := src/main.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
