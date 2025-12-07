CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17

SRC = $(wildcard *.cpp)
OBJ = $(patsubst %.cpp, build/%.o, $(SRC))  # Object files in build/ directory
TARGET = dax

# Create build/ directory if it doesn't exist
$(shell mkdir -p build)

# Declare the target and its dependencies
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJ) -lreadline

# Rule for generating object files from source files
build/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) build/*.o
