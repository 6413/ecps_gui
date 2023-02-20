FLAGS = -Wno-shift-op-parentheses -Wno-unused-variable -Wno-unused-but-set-variable -Wno-macro-redefined -Wno-unused-private-field -Wno-unused-function
LINK = -lev -lpthread -lcrypto -lssl -lxcb -lxcb-shm -lxcb-keysyms -lxcb-xtest -lGL -lX11 -lXtst -lXfixes -lXrandr -lwebp -lopenh264 -lx264 /usr/local/lib/fan/fan.a
PARAM = $(FLAGS) $(LINK)

debug:
	lclang++ -gdwarf-4 ecps_client.cpp $(PARAM) -o ecps_client.exe

release:
	lclang++-size ecps_client.cpp -o ecps_client.exe $(PARAM) -s -O3 -march=native -mtune=native
