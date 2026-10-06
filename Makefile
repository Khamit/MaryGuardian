CXX      := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror=return-type -fstack-protector-strong -D_FORTIFY_SOURCE=2 -MMD -MP

MUPDF_DIR := /opt/homebrew/opt/mupdf

INCLUDES := \
    -I. \
    -I$(MUPDF_DIR)/include/mupdf \
    -I$(MUPDF_DIR)/include \
    -Ithird_party/llama.cpp/include \
    -Ithird_party/llama.cpp/ggml/include \
    -Ithird_party/llama.cpp/common \
    -Ithird_party \
    -Ithird_party/llama.cpp/tools/mtmd

LLAMA_DIR  := third_party/llama.cpp
LLAMA_BIN  := $(LLAMA_DIR)/build/bin

LLAMA_LIBS := \
    -L$(LLAMA_BIN) \
    -L$(MUPDF_DIR)/lib \
    -lllama -lllama-common \
    -lggml -lggml-base -lggml-cpu -lggml-metal \
    -lmtmd \
    -lmupdf -lmupdf-third \
    -lsqlite3

FRAMEWORKS := \
    -framework Accelerate \
    -framework Metal \
    -framework MetalKit \
    -framework Foundation

SOURCES := \
    main.cpp \
    core/ModelRuntime.cpp \
    core/PdfToolEngine.cpp \
    core/DocReader.cpp \
    core/Guardian.cpp \
    core/Verifier.cpp \
    core/Db.cpp \
    core/Extract.cpp \
    core/Indexer.cpp \
    core/Library.cpp \
    core/OcrEngine.cpp \
    task/BookkeeperTask.cpp \
    server/HttpServer.cpp

OBJECTS := $(SOURCES:.cpp=.o)
TARGET  := MaryGuardian

.PHONY: all clean test install-web
all: $(TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ \
	    $(LLAMA_LIBS) $(FRAMEWORKS) \
	    -lpthread \
	    -Wl,-rpath,@loader_path/$(LLAMA_BIN) \
	    -Wl,-rpath,$(MUPDF_DIR)/lib

test: tests/guardian_test.cpp core/Guardian.cpp core/Verifier.cpp
	$(CXX) $(CXXFLAGS) -I. -Ithird_party -o guardian_test $^ && ./guardian_test

install-web:
	mkdir -p mg_workspace/web
	cp web/index.html mg_workspace/web/index.html

-include $(OBJECTS:.o=.d)

clean:
	rm -f $(TARGET) guardian_test $(OBJECTS) $(OBJECTS:.o=.d)