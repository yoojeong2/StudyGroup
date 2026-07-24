# 오프라인(폐쇄망) 빌드 - 패키지 매니저/인터넷 불필요.
# 헤더 온리 라이브러리(third_party/httplib.h, third_party/json.hpp)만 사용.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -pthread
TARGET   ?= server

$(TARGET): main.cpp third_party/httplib.h third_party/json.hpp
	$(CXX) $(CXXFLAGS) main.cpp -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: run clean
