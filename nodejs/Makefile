CPP_SHARED := -DUSE_LIBUV -std=c++17 -O3 -Wno-deprecated-declarations -I ../uWebSockets/src -shared -fPIC ../uWebSockets/src/Extensions.cpp ../uWebSockets/src/Group.cpp ../uWebSockets/src/Networking.cpp ../uWebSockets/src/Hub.cpp ../uWebSockets/src/Node.cpp ../uWebSockets/src/WebSocket.cpp ../uWebSockets/src/HTTPSocket.cpp ../uWebSockets/src/Socket.cpp ../uWebSockets/src/Epoll.cpp src/addon.cpp

default:
	make targets
	NODE=targets/node-v12.13.1 ABI=72 make `(uname -s)`
	NODE=targets/node-v18.4.0 ABI=108 make `(uname -s)`
	cp README.md dist/README.md
	cp ../uWebSockets/LICENSE dist/LICENSE
	cp -r ../uWebSockets/src dist/
	cp src/addon.cpp dist/src/addon.cpp
	cp src/addon.h dist/src/addon.h
	cp src/http.h dist/src/http.h
	cp src/uws.js dist/uws.js
	for f in dist/*.node; do chmod +x $$f; done
targets:
	mkdir targets
	curl https://nodejs.org/dist/v12.13.1/node-v12.13.1-headers.tar.gz | tar xz -C targets
	curl https://nodejs.org/dist/v18.4.0/node-v18.4.0-headers.tar.gz | tar xz -C targets
Linux:
	g++ $(CPP_SHARED) -static-libstdc++ -static-libgcc -I $$NODE/include/node -s -o dist/uws_linux_$$ABI.node
.PHONY: clean
clean:
	rm -f dist/README.md
	rm -f dist/LICENSE
	rm -f dist/uws_*.node
	rm -f dist/uws.js
	rm -rf dist/src
	rm -rf targets
