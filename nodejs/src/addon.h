#include <node.h>
#include <node_buffer.h>
#include <cstring>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <uv.h>

using namespace std;
using namespace v8;

uWS::Hub hub(0, true);
uv_check_t check;
Persistent<Function> noop;

void registerCheck(Isolate *isolate) {
    uv_check_init((uv_loop_t *) hub.getLoop(), &check);
    check.data = isolate;
    uv_check_start(&check, [](uv_check_t *check) {
        Isolate *isolate = (Isolate *) check->data;
        HandleScope hs(isolate);
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, noop), 0, nullptr);
    });
    uv_unref((uv_handle_t *) &check);
}

class NativeString {
    char *data;
    size_t length;
    char utf8ValueMemory[sizeof(String::Utf8Value)];
    String::Utf8Value *utf8Value = nullptr;
public:
    NativeString(const Local<Value> &value) {
        if (value->IsUndefined()) {
            data = nullptr;
            length = 0;
        } else if (value->IsString()) {
            #if V8_MAJOR_VERSION >= 7
            utf8Value = new (utf8ValueMemory) String::Utf8Value(v8::Isolate::GetCurrent(), value);
            #else
            utf8Value = new (utf8ValueMemory) String::Utf8Value(value);
            #endif
            data = (**utf8Value);
            length = utf8Value->length();
        } else if (node::Buffer::HasInstance(value)) {
            data = node::Buffer::Data(value);
            length = node::Buffer::Length(value);
        } else if (value->IsTypedArray()) {
            #if V8_MAJOR_VERSION >= 10
            Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(value);
            auto backingStore = arrayBufferView->Buffer()->GetBackingStore();
            length = backingStore->ByteLength();
            data = (char *) backingStore->Data();
            #else
            Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(value);
            ArrayBuffer::Contents contents = arrayBufferView->Buffer()->GetContents();
            length = contents.ByteLength();
            data = (char *) contents.Data();
            #endif
        } else if (value->IsArrayBuffer()) {
            #if V8_MAJOR_VERSION >= 10
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
            auto backingStore = arrayBuffer->GetBackingStore();
            length = backingStore->ByteLength();
            data = (char *) backingStore->Data();
            #else
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
            ArrayBuffer::Contents contents = arrayBuffer->GetContents();
            length = contents.ByteLength();
            data = (char *) contents.Data();
            #endif
        } else {
            static char empty[] = "";
            data = empty;
            length = 0;
        }
    }

    char *getData() {return data;}
    size_t getLength() {return length;}
    ~NativeString() {
        if (utf8Value) {
            utf8Value->~Utf8Value();
        }
    }
};

struct GroupData {
    Persistent<Function> connectionHandler, messageHandler,
                         disconnectionHandler, pingHandler,
                         pongHandler, errorHandler, httpRequestHandler,
                         httpUpgradeHandler, httpCancelledRequestCallback;
    int size = 0;
};

template <bool isServer>
void createGroup(const FunctionCallbackInfo<Value> &args) {
    #if V8_MAJOR_VERSION >= 7
    uWS::Group<isServer> *group = hub.createGroup<isServer>(
        args[0]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust(),
        args[1]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust()
    );
    #else
    uWS::Group<isServer> *group = hub.createGroup<isServer>(args[0]->IntegerValue(), args[1]->IntegerValue());
    #endif
    group->setUserData(new GroupData);
    args.GetReturnValue().Set(External::New(args.GetIsolate(), group));
}

template <bool isServer>
void deleteGroup(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<isServer> *group = (uWS::Group<isServer> *) args[0].As<External>()->Value();
    delete (GroupData *) group->getUserData();
    delete group;
}

template <bool isServer>
inline Local<External> wrapSocket(uWS::WebSocket<isServer> *webSocket, Isolate *isolate) {
    return External::New(isolate, webSocket);
}

template <bool isServer>
inline uWS::WebSocket<isServer> *unwrapSocket(Local<External> external) {
    return (uWS::WebSocket<isServer> *) external->Value();
}

inline Local<Value> wrapMessage(const char *message, size_t length, uWS::OpCode opCode, Isolate *isolate) {
    #if V8_MAJOR_VERSION >= 10
    if (opCode == uWS::OpCode::BINARY) {
        std::unique_ptr<v8::BackingStore> backing = ArrayBuffer::NewBackingStore(
            (char *) message, length, [](void*, size_t, void*){}, nullptr);
        return ArrayBuffer::New(isolate, std::move(backing));
    } else {
        return String::NewFromUtf8(isolate, message, NewStringType::kNormal, length).ToLocalChecked();
    }
    #else
    return opCode == uWS::OpCode::BINARY ?
        (Local<Value>) ArrayBuffer::New(isolate, (char *) message, length) :
        (Local<Value>) String::NewFromUtf8(isolate, message, String::kNormalString, length);
    #endif
}

template <bool isServer>
inline Local<Value> getDataV8(uWS::WebSocket<isServer> *webSocket, Isolate *isolate) {
    return webSocket->getUserData() ? Local<Value>::New(isolate, *(Persistent<Value> *) webSocket->getUserData()) : Local<Value>::Cast(Undefined(isolate));
}

template <bool isServer>
void getUserData(const FunctionCallbackInfo<Value> &args) {
    args.GetReturnValue().Set(getDataV8(unwrapSocket<isServer>(args[0].As<External>()), args.GetIsolate()));
}

template <bool isServer>
void clearUserData(const FunctionCallbackInfo<Value> &args) {
    uWS::WebSocket<isServer> *webSocket = unwrapSocket<isServer>(args[0].As<External>());
    ((Persistent<Value> *) webSocket->getUserData())->Reset();
    delete (Persistent<Value> *) webSocket->getUserData();
}

template <bool isServer>
void setUserData(const FunctionCallbackInfo<Value> &args) {
    uWS::WebSocket<isServer> *webSocket = unwrapSocket<isServer>(args[0].As<External>());
    if (webSocket->getUserData()) {
        ((Persistent<Value> *) webSocket->getUserData())->Reset(args.GetIsolate(), args[1]);
    } else {
        webSocket->setUserData(new Persistent<Value>(args.GetIsolate(), args[1]));
    }
}

template <bool isServer>
void getAddress(const FunctionCallbackInfo<Value> &args)
{
    typename uWS::WebSocket<isServer>::Address address = unwrapSocket<isServer>(args[0].As<External>())->getAddress();
    Local<Array> array = Array::New(args.GetIsolate(), 3);
    #if V8_MAJOR_VERSION >= 10
    array->Set(args.GetIsolate()->GetCurrentContext(), 0, Integer::New(args.GetIsolate(), address.port));
    array->Set(args.GetIsolate()->GetCurrentContext(), 1, String::NewFromUtf8(args.GetIsolate(), address.address).ToLocalChecked());
    array->Set(args.GetIsolate()->GetCurrentContext(), 2, String::NewFromUtf8(args.GetIsolate(), address.family).ToLocalChecked());
    #else
    array->Set(0, Integer::New(args.GetIsolate(), address.port));
    array->Set(1, String::NewFromUtf8(args.GetIsolate(), address.address));
    array->Set(2, String::NewFromUtf8(args.GetIsolate(), address.family));
    #endif
    args.GetReturnValue().Set(array);
}

uv_handle_t *getTcpHandle(void *handleWrap) {
    volatile char *memory = (volatile char *) handleWrap;
    for (volatile uv_handle_t *tcpHandle = (volatile uv_handle_t *) memory; tcpHandle->type != UV_TCP
         || tcpHandle->data != handleWrap || tcpHandle->loop != uv_default_loop(); tcpHandle = (volatile uv_handle_t *) memory) {
        memory++;
    }
    return (uv_handle_t *) memory;
}

struct SendCallbackData {
    Persistent<Function> jsCallback;
    Isolate *isolate;
};

template <bool isServer>
void sendCallback(uWS::WebSocket<isServer> *webSocket, void *data, bool cancelled, void *reserved)
{
    SendCallbackData *sc = (SendCallbackData *) data;
    if (!cancelled) {
        HandleScope hs(sc->isolate);
        node::MakeCallback(sc->isolate, sc->isolate->GetCurrentContext()->Global(), Local<Function>::New(sc->isolate, sc->jsCallback), 0, nullptr);
    }
    sc->jsCallback.Reset();
    delete sc;
}

template <bool isServer>
void send(const FunctionCallbackInfo<Value> &args)
{
    #if V8_MAJOR_VERSION >= 7
    uWS::OpCode opCode = (uWS::OpCode) args[2]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust();
    #else
    uWS::OpCode opCode = (uWS::OpCode) args[2]->IntegerValue();
    #endif
    NativeString nativeString(args[1]);

    SendCallbackData *sc = nullptr;
    void (*callback)(uWS::WebSocket<isServer> *, void *, bool, void *) = nullptr;

    if (args[3]->IsFunction()) {
        callback = sendCallback;
        sc = new SendCallbackData;
        sc->jsCallback.Reset(args.GetIsolate(), Local<Function>::Cast(args[3]));
        sc->isolate = args.GetIsolate();
    }
    #if V8_MAJOR_VERSION >= 10
    bool compress = args[4]->BooleanValue(args.GetIsolate());
    #elif V8_MAJOR_VERSION >= 7
    bool compress = args[4]->BooleanValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust();
    #else
    bool compress = args[4]->BooleanValue();
    #endif

    unwrapSocket<isServer>(args[0].As<External>())->send(nativeString.getData(),
                           nativeString.getLength(), opCode, callback, sc, compress);
}

void connect(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<uWS::CLIENT> *clientGroup = (uWS::Group<uWS::CLIENT> *) args[0].As<External>()->Value();
    NativeString uri(args[1]);
    hub.connect(std::string(uri.getData(), uri.getLength()), new Persistent<Value>(args.GetIsolate(), args[2]), {}, 5000, clientGroup);
}

struct Ticket {
    uv_os_sock_t fd;
    SSL *ssl;
};

void upgrade(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<uWS::SERVER> *serverGroup = (uWS::Group<uWS::SERVER> *) args[0].As<External>()->Value();
    Ticket *ticket = (Ticket *) args[1].As<External>()->Value();
    NativeString secKey(args[2]);
    NativeString extensions(args[3]);
    NativeString subprotocol(args[4]);

    // todo: move this check into core!
    if (ticket->fd != INVALID_SOCKET) {
        hub.upgrade(ticket->fd, secKey.getData(), ticket->ssl, extensions.getData(), extensions.getLength(), subprotocol.getData(), subprotocol.getLength(), serverGroup);
    } else {
        if (ticket->ssl) {
            SSL_free(ticket->ssl);
        }
    }
    delete ticket;
}

void transfer(const FunctionCallbackInfo<Value> &args) {
    // (_handle.fd OR _handle), SSL
    uv_handle_t *handle = nullptr;
    Ticket *ticket = new Ticket;
    if (args[0]->IsObject()) {
        #if V8_MAJOR_VERSION >= 7
        uv_fileno(
            (handle = getTcpHandle(args[0]->ToObject(v8::Isolate::GetCurrent()->GetCurrentContext()).ToLocalChecked()->GetAlignedPointerFromInternalField(0))),
            (uv_os_fd_t *) &ticket->fd
        );
        #else
        uv_fileno((handle = getTcpHandle(args[0]->ToObject()->GetAlignedPointerFromInternalField(0))), (uv_os_fd_t *) &ticket->fd);
        #endif
    } else {
        #if V8_MAJOR_VERSION >= 7
        ticket->fd = args[0]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust();
        #else
        ticket->fd = args[0]->IntegerValue();
        #endif
    }

    ticket->fd = dup(ticket->fd);
    ticket->ssl = nullptr;
    if (args[1]->IsExternal()) {
        ticket->ssl = (SSL *) args[1].As<External>()->Value();
        SSL_up_ref(ticket->ssl);
    }

    // uv_close calls shutdown if not set on Windows
    if (handle) {
        // UV_HANDLE_SHARED_TCP_SOCKET
        handle->flags |= 0x40000000;
    }

    args.GetReturnValue().Set(External::New(args.GetIsolate(), ticket));
}

template <bool isServer>
void onConnection(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<isServer> *group = (uWS::Group<isServer> *) args[0].As<External>()->Value();
    GroupData *groupData = (GroupData *) group->getUserData();

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *connectionCallback = &groupData->connectionHandler;
    connectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onConnection([isolate, connectionCallback, groupData](uWS::WebSocket<isServer> *webSocket, uWS::HttpRequest req) {
        groupData->size++;
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapSocket(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *connectionCallback), 1, argv);
    });
}

template <bool isServer>
void onMessage(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<isServer> *group = (uWS::Group<isServer> *) args[0].As<External>()->Value();
    GroupData *groupData = (GroupData *) group->getUserData();

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *messageCallback = &groupData->messageHandler;
    messageCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onMessage([isolate, messageCallback](uWS::WebSocket<isServer> *webSocket, const char *message, size_t length, uWS::OpCode opCode) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapMessage(message, length, opCode, isolate),
                               getDataV8(webSocket, isolate)};
        #if V8_MAJOR_VERSION >= 7
        Local<Function>::New(isolate, *messageCallback)->Call(isolate->GetCurrentContext(), isolate->GetCurrentContext()->Global(), 2, argv);
        #else
        Local<Function>::New(isolate, *messageCallback)->Call(isolate->GetCurrentContext()->Global(), 2, argv);
        #endif
    });
}

template <bool isServer>
void onPing(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<isServer> *group = (uWS::Group<isServer> *) args[0].As<External>()->Value();
    GroupData *groupData = (GroupData *) group->getUserData();

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *pingCallback = &groupData->pingHandler;
    pingCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onPing([isolate, pingCallback](uWS::WebSocket<isServer> *webSocket, const char *message, size_t length) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapMessage(message, length, uWS::OpCode::PING, isolate),
                               getDataV8(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *pingCallback), 2, argv);
    });
}

template <bool isServer>
void onPong(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<isServer> *group = (uWS::Group<isServer> *) args[0].As<External>()->Value();
    GroupData *groupData = (GroupData *) group->getUserData();

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *pongCallback = &groupData->pongHandler;
    pongCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onPong([isolate, pongCallback](uWS::WebSocket<isServer> *webSocket, const char *message, size_t length) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapMessage(message, length, uWS::OpCode::PONG, isolate),
                               getDataV8(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *pongCallback), 2, argv);
    });
}

template <bool isServer>
void onDisconnection(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<isServer> *group = (uWS::Group<isServer> *) args[0].As<External>()->Value();
    GroupData *groupData = (GroupData *) group->getUserData();

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *disconnectionCallback = &groupData->disconnectionHandler;
    disconnectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));

    group->onDisconnection([isolate, disconnectionCallback, groupData](uWS::WebSocket<isServer> *webSocket, int code, char *message, size_t length) {
        groupData->size--;
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapSocket(webSocket, isolate),
                               Integer::New(isolate, code),
                               wrapMessage(message, length, uWS::OpCode::CLOSE, isolate),
                               getDataV8(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *disconnectionCallback), 4, argv);
    });
}

void onError(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<uWS::CLIENT> *group = (uWS::Group<uWS::CLIENT> *) args[0].As<External>()->Value();
    GroupData *groupData = (GroupData *) group->getUserData();

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *errorCallback = &groupData->errorHandler;
    errorCallback->Reset(isolate, Local<Function>::Cast(args[1]));

    group->onError([isolate, errorCallback](void *user) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {Local<Value>::New(isolate, *(Persistent<Value> *) user)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, *errorCallback), 1, argv);

        ((Persistent<Value> *) user)->Reset();
        delete (Persistent<Value> *) user;
    });
}

template <bool isServer>
void closeSocket(const FunctionCallbackInfo<Value> &args) {
    NativeString nativeString(args[2]);
    #if V8_MAJOR_VERSION >= 7
    unwrapSocket<isServer>(args[0].As<External>())->close(
        args[1]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust(),
        nativeString.getData(),
        nativeString.getLength()
    );
    #else
    unwrapSocket<isServer>(args[0].As<External>())->close(args[1]->IntegerValue(), nativeString.getData(), nativeString.getLength());
    #endif
}

template <bool isServer>
void terminateSocket(const FunctionCallbackInfo<Value> &args) {
    unwrapSocket<isServer>(args[0].As<External>())->terminate();
}

template <bool isServer>
void closeGroup(const FunctionCallbackInfo<Value> &args) {
    NativeString nativeString(args[2]);
    uWS::Group<isServer> *group = (uWS::Group<isServer> *) args[0].As<External>()->Value();
    #if V8_MAJOR_VERSION >= 7
    group->close(
        args[1]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust(),
        nativeString.getData(),
        nativeString.getLength()
    );
    #else
    group->close(args[1]->IntegerValue(), nativeString.getData(), nativeString.getLength());
    #endif
}

template <bool isServer>
void terminateGroup(const FunctionCallbackInfo<Value> &args) {
    ((uWS::Group<isServer> *) args[0].As<External>()->Value())->terminate();
}

template <bool isServer>
void broadcast(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<isServer> *group = (uWS::Group<isServer> *) args[0].As<External>()->Value();
    #if V8_MAJOR_VERSION >= 10
    uWS::OpCode opCode = args[2]->BooleanValue(args.GetIsolate())
        ? uWS::OpCode::BINARY
        : uWS::OpCode::TEXT;
    #elif V8_MAJOR_VERSION >= 7
    uWS::OpCode opCode = args[2]->BooleanValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust()
        ? uWS::OpCode::BINARY
        : uWS::OpCode::TEXT;
    #else
    uWS::OpCode opCode = args[2]->BooleanValue() ? uWS::OpCode::BINARY : uWS::OpCode::TEXT;
    #endif
    NativeString nativeString(args[1]);
    group->broadcast(nativeString.getData(), nativeString.getLength(), opCode);
}

template <bool isServer>
void prepareMessage(const FunctionCallbackInfo<Value> &args) {
    #if V8_MAJOR_VERSION >= 7
    uWS::OpCode opCode = (uWS::OpCode) args[1]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust();
    #else
    uWS::OpCode opCode = (uWS::OpCode) args[1]->IntegerValue();
    #endif
    NativeString nativeString(args[0]);
    args.GetReturnValue().Set(External::New(args.GetIsolate(), uWS::WebSocket<isServer>::prepareMessage(nativeString.getData(), nativeString.getLength(), opCode, false)));
}

template <bool isServer>
void sendPrepared(const FunctionCallbackInfo<Value> &args) {
    unwrapSocket<isServer>(args[0].As<External>())
        ->sendPrepared((typename uWS::WebSocket<isServer>::PreparedMessage *) args[1].As<External>()->Value());
}

template <bool isServer>
void finalizeMessage(const FunctionCallbackInfo<Value> &args) {
    uWS::WebSocket<isServer>::finalizeMessage((typename uWS::WebSocket<isServer>::PreparedMessage *) args[0].As<External>()->Value());
}

void forEach(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    uWS::Group<uWS::SERVER> *group = (uWS::Group<uWS::SERVER> *) args[0].As<External>()->Value();
    Local<Function> cb = Local<Function>::Cast(args[1]);
    group->forEach([isolate, &cb](uWS::WebSocket<uWS::SERVER> *webSocket) {
        Local<Value> argv[] = {
            getDataV8(webSocket, isolate)
        };
        #if V8_MAJOR_VERSION >= 7
        cb->Call(v8::Isolate::GetCurrent()->GetCurrentContext(), Null(isolate), 1, argv);
        #else
        cb->Call(Null(isolate), 1, argv);
        #endif
    });
}

void getSize(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<uWS::SERVER> *group = (uWS::Group<uWS::SERVER> *) args[0].As<External>()->Value();
    GroupData *groupData = (GroupData *) group->getUserData();
    args.GetReturnValue().Set(Integer::New(args.GetIsolate(), groupData->size));
}

void startAutoPing(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<uWS::SERVER> *group = (uWS::Group<uWS::SERVER> *) args[0].As<External>()->Value();
    NativeString nativeString(args[2]);
    #if V8_MAJOR_VERSION >= 7
    group->startAutoPing(
        args[1]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust(),
        std::string(nativeString.getData(), nativeString.getLength())
    );
    #else
    group->startAutoPing(args[1]->IntegerValue(), std::string(nativeString.getData(), nativeString.getLength()));
    #endif
}

void setNoop(const FunctionCallbackInfo<Value> &args) {
    noop.Reset(args.GetIsolate(), Local<Function>::Cast(args[0]));
}

void listen(const FunctionCallbackInfo<Value> &args) {
    uWS::Group<uWS::SERVER> *group = (uWS::Group<uWS::SERVER> *) args[0].As<External>()->Value();
    #if V8_MAJOR_VERSION >= 7
    hub.listen(
        args[1]->IntegerValue(v8::Isolate::GetCurrent()->GetCurrentContext()).FromJust(),
        nullptr, 0, group
    );
    #else
    hub.listen(args[1]->IntegerValue(), nullptr, 0, group);
    #endif
}

template <bool isServer>
struct Namespace {
    Local<Object> object;
    Namespace (Isolate *isolate) {
        object = Object::New(isolate);
        NODE_SET_METHOD(object, "send", send<isServer>);
        NODE_SET_METHOD(object, "close", closeSocket<isServer>);
        NODE_SET_METHOD(object, "terminate", terminateSocket<isServer>);
        NODE_SET_METHOD(object, "prepareMessage", prepareMessage<isServer>);
        NODE_SET_METHOD(object, "sendPrepared", sendPrepared<isServer>);
        NODE_SET_METHOD(object, "finalizeMessage", finalizeMessage<isServer>);

        Local<Object> group = Object::New(isolate);
        NODE_SET_METHOD(group, "onConnection", onConnection<isServer>);
        NODE_SET_METHOD(group, "onMessage", onMessage<isServer>);
        NODE_SET_METHOD(group, "onDisconnection", onDisconnection<isServer>);

        if (!isServer) {
            NODE_SET_METHOD(group, "onError", onError);
        } else {
            NODE_SET_METHOD(group, "forEach", forEach);
            NODE_SET_METHOD(group, "getSize", getSize);
            NODE_SET_METHOD(group, "startAutoPing", startAutoPing);
            NODE_SET_METHOD(group, "listen", listen);
        }

        NODE_SET_METHOD(group, "onPing", onPing<isServer>);
        NODE_SET_METHOD(group, "onPong", onPong<isServer>);
        NODE_SET_METHOD(group, "create", createGroup<isServer>);
        NODE_SET_METHOD(group, "delete", deleteGroup<isServer>);
        NODE_SET_METHOD(group, "close", closeGroup<isServer>);
        NODE_SET_METHOD(group, "terminate", terminateGroup<isServer>);
        NODE_SET_METHOD(group, "broadcast", broadcast<isServer>);

        #if V8_MAJOR_VERSION >= 10
        object->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "group").ToLocalChecked(), group);
        #else
        object->Set(String::NewFromUtf8(isolate, "group"), group);
        #endif
    }
};
