CXX=g++
CXXFLAGS=-Wall -Wextra -Werror -std=c++20 -g -O3

build/Linux/sfinder.so: sfinder.cpp
	mkdir -p $(@D)
	@stat $(JAVA_HOME)/include/linux/jni_md.h >/dev/null 2>&1 || { echo "ERROR: JAVA_HOME is not set or is invalid"; exit 1; }
	$(CXX) $(CXXFLAGS) -shared -fPIC $^ -o $@ \
		-I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -L$(JAVA_HOME)/jre/lib/amd64/server -ljvm \
		-I/usr/include/luajit-2.1 -lluajit-5.1