// ipc_codegen - Steam IPC IDL to C++ generator for OpenSteamTool.
//
// Usage: ipc_codegen <input.steamd> <output.gen.h>
//
// The generated header contains the IPC enums, transport envelope wrappers,
// and per-method Req/Resp facades. The tool intentionally has no external
// dependencies and is built as a small C++20 host utility.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum class Dir { In, Out };

struct Field {
    Dir         dir = Dir::In;
    std::string type;
    std::string name;
    std::string lenRef;
    int         line = 0;
};

struct EnumMember {
    std::string name;
    std::string value;
    int         line = 0;
};

struct EnumDecl {
    std::string             name;
    std::string             underlying;
    std::vector<EnumMember> members;
    int                     line = 0;
};

struct StructDecl {
    std::string        name;
    std::vector<Field> fields;
    int                line = 0;
};

struct FrameField {
    bool        payload = false;
    std::string type;
    std::string name;
    int         line = 0;
};

struct FrameDecl {
    std::vector<FrameField> fields;
};

struct CommandDecl {
    std::string name;
    std::string enumValue;
    FrameDecl   request;
    FrameDecl   response;
    bool        responseNone = false;
    int         line = 0;
};

struct ProtocolDecl {
    std::string              name;
    std::string              requestHeader;
    std::vector<CommandDecl> commands;
    int                      line = 0;
};

struct Method {
    std::string        name;
    std::string        retType;
    std::vector<Field> params;
    int                line = 0;
};

struct Interface {
    std::string         name;
    std::vector<Method> methods;
    int                 line = 0;
};

struct File {
    std::vector<EnumDecl>     enums;
    std::vector<StructDecl>   structs;
    std::vector<ProtocolDecl> protocols;
    std::vector<Interface>    interfaces;
};

enum class Tok {
    Ident,
    Int,
    LBrace,
    RBrace,
    LParen,
    RParen,
    LBrack,
    RBrack,
    Comma,
    Semi,
    Colon,
    Scope,
    Equal,
    End,
};

struct Token {
    Tok         kind;
    std::string text;
    int         line = 0;
    int         col = 0;
};

class Lexer {
public:
    Lexer(std::string_view src, std::string filename)
        : src_(src), filename_(std::move(filename)) {}

    Token next() {
        skipWs();
        const int startLine = line_;
        const int startCol = column();
        if (pos_ >= src_.size()) return {Tok::End, "", startLine, startCol};

        const char c = src_[pos_];
        if (c == '{') { ++pos_; return {Tok::LBrace, "{", startLine, startCol}; }
        if (c == '}') { ++pos_; return {Tok::RBrace, "}", startLine, startCol}; }
        if (c == '(') { ++pos_; return {Tok::LParen, "(", startLine, startCol}; }
        if (c == ')') { ++pos_; return {Tok::RParen, ")", startLine, startCol}; }
        if (c == '[') { ++pos_; return {Tok::LBrack, "[", startLine, startCol}; }
        if (c == ']') { ++pos_; return {Tok::RBrack, "]", startLine, startCol}; }
        if (c == ',') { ++pos_; return {Tok::Comma, ",", startLine, startCol}; }
        if (c == ';') { ++pos_; return {Tok::Semi, ";", startLine, startCol}; }
        if (c == '=') { ++pos_; return {Tok::Equal, "=", startLine, startCol}; }
        if (c == ':' && pos_ + 1 < src_.size() && src_[pos_ + 1] == ':') {
            pos_ += 2;
            return {Tok::Scope, "::", startLine, startCol};
        }
        if (c == ':') { ++pos_; return {Tok::Colon, ":", startLine, startCol}; }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const size_t start = pos_++;
            while (pos_ < src_.size()) {
                const char next = src_[pos_];
                if (!std::isalnum(static_cast<unsigned char>(next)) && next != '_') break;
                ++pos_;
            }
            return {Tok::Ident, std::string(src_.substr(start, pos_ - start)), startLine, startCol};
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            const size_t start = pos_++;
            while (pos_ < src_.size() &&
                   std::isalnum(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
            return {Tok::Int, std::string(src_.substr(start, pos_ - start)), startLine, startCol};
        }

        die(startLine, startCol, "unexpected character '" + std::string(1, c) + "'");
    }

    [[noreturn]] void die(int line, int col, const std::string& message) const {
        std::fprintf(stderr, "%s:%d:%d: error: %s\n",
                     filename_.c_str(), line, col, message.c_str());
        std::exit(1);
    }

private:
    void skipWs() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '\n') {
                ++line_;
                ++pos_;
                lineStart_ = pos_;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < src_.size() &&
                       !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
                    if (src_[pos_] == '\n') {
                        ++line_;
                        lineStart_ = pos_ + 1;
                    }
                    ++pos_;
                }
                if (pos_ + 1 >= src_.size()) die(line_, column(), "unterminated block comment");
                pos_ += 2;
            } else {
                break;
            }
        }
    }

    int column() const { return static_cast<int>(pos_ - lineStart_) + 1; }

    std::string_view src_;
    std::string      filename_;
    size_t           pos_ = 0;
    size_t           lineStart_ = 0;
    int              line_ = 1;
};

class Parser {
public:
    explicit Parser(Lexer& lexer) : lexer_(lexer), token_(lexer.next()) {}

    File parse() {
        File file;
        while (token_.kind != Tok::End) {
            const Token kind = expect(Tok::Ident, "declaration");
            if (kind.text == "enum") {
                file.enums.push_back(parseEnum());
            } else if (kind.text == "struct") {
                file.structs.push_back(parseStruct());
            } else if (kind.text == "protocol") {
                file.protocols.push_back(parseProtocol());
            } else if (kind.text == "interface") {
                file.interfaces.push_back(parseInterface());
            } else {
                fail(kind.line, "unknown declaration '" + kind.text + "'");
            }
        }
        validate(file);
        return file;
    }

private:
    EnumDecl parseEnum() {
        EnumDecl decl;
        decl.line = token_.line;
        decl.name = expect(Tok::Ident, "enum name").text;
        expect(Tok::Colon);
        decl.underlying = expect(Tok::Ident, "enum underlying type").text;
        expect(Tok::LBrace);
        std::unordered_set<std::string> names;
        while (token_.kind != Tok::RBrace) {
            EnumMember member;
            member.line = token_.line;
            member.name = expect(Tok::Ident, "enum member").text;
            ensureUnique(names, member.name, "enum member", member.line);
            expect(Tok::Equal);
            member.value = expect(Tok::Int, "enum value").text;
            expect(Tok::Semi);
            decl.members.push_back(std::move(member));
        }
        expect(Tok::RBrace);
        optionalSemi();
        return decl;
    }

    StructDecl parseStruct() {
        StructDecl decl;
        decl.line = token_.line;
        decl.name = expect(Tok::Ident, "struct name").text;
        expect(Tok::LBrace);
        std::unordered_set<std::string> names;
        while (token_.kind != Tok::RBrace) {
            Field field = parseField(false);
            ensureUnique(names, field.name, "struct field", field.line);
            expect(Tok::Semi);
            decl.fields.push_back(std::move(field));
        }
        expect(Tok::RBrace);
        optionalSemi();
        return decl;
    }

    ProtocolDecl parseProtocol() {
        ProtocolDecl decl;
        decl.line = token_.line;
        decl.name = expect(Tok::Ident, "protocol name").text;
        expect(Tok::LBrace);
        std::unordered_set<std::string> commandNames;
        while (token_.kind != Tok::RBrace) {
            const Token keyword = expect(Tok::Ident, "'request' or 'command'");
            if (keyword.text == "request") {
                if (!decl.requestHeader.empty()) fail(keyword.line, "duplicate protocol request header");
                decl.requestHeader = expect(Tok::Ident, "request header type").text;
                expect(Tok::Semi);
                continue;
            }
            if (keyword.text != "command") {
                fail(keyword.line, "expected 'request' or 'command', got '" + keyword.text + "'");
            }
            CommandDecl command;
            command.line = token_.line;
            command.name = expect(Tok::Ident, "command name").text;
            ensureUnique(commandNames, command.name, "command", command.line);
            expect(Tok::Equal);
            command.enumValue = parseScopedName();
            expect(Tok::LBrace);
            bool hasRequest = false;
            bool hasResponse = false;
            while (token_.kind != Tok::RBrace) {
                const Token section = expect(Tok::Ident, "'request' or 'response'");
                if (section.text == "request") {
                    if (hasRequest) fail(section.line, "duplicate command request section");
                    command.request = parseFrame();
                    hasRequest = true;
                } else if (section.text == "response") {
                    if (hasResponse) fail(section.line, "duplicate command response section");
                    if (token_.kind == Tok::Ident && token_.text == "none") {
                        command.responseNone = true;
                        advance();
                        expect(Tok::Semi);
                    } else {
                        command.response = parseFrame();
                    }
                    hasResponse = true;
                } else {
                    fail(section.line, "expected 'request' or 'response', got '" + section.text + "'");
                }
            }
            expect(Tok::RBrace);
            optionalSemi();
            if (!hasRequest || !hasResponse) fail(command.line, "command requires request and response sections");
            decl.commands.push_back(std::move(command));
        }
        expect(Tok::RBrace);
        optionalSemi();
        return decl;
    }

    FrameDecl parseFrame() {
        FrameDecl frame;
        expect(Tok::LBrace);
        std::unordered_set<std::string> names;
        bool hasPayload = false;
        while (token_.kind != Tok::RBrace) {
            FrameField field;
            field.line = token_.line;
            field.type = expect(Tok::Ident, "frame field type").text;
            field.payload = field.type == "payload";
            field.name = expect(Tok::Ident, "frame field name").text;
            ensureUnique(names, field.name, "frame field", field.line);
            if (field.payload) {
                if (hasPayload) fail(field.line, "frame may contain at most one payload");
                hasPayload = true;
            }
            expect(Tok::Semi);
            frame.fields.push_back(std::move(field));
        }
        expect(Tok::RBrace);
        return frame;
    }

    Interface parseInterface() {
        Interface iface;
        iface.line = token_.line;
        iface.name = expect(Tok::Ident, "interface name").text;
        expect(Tok::LBrace);
        std::unordered_set<std::string> methodNames;
        while (token_.kind != Tok::RBrace) {
            Method method = parseMethod();
            ensureUnique(methodNames, method.name, "method", method.line);
            iface.methods.push_back(std::move(method));
        }
        expect(Tok::RBrace);
        optionalSemi();
        return iface;
    }

    Method parseMethod() {
        Method method;
        method.line = token_.line;
        method.retType = expect(Tok::Ident, "return type").text;
        method.name = expect(Tok::Ident, "method name").text;
        expect(Tok::LParen);
        std::unordered_set<std::string> names;
        if (token_.kind != Tok::RParen) {
            for (;;) {
                Field field = parseField(true);
                ensureUnique(names, field.name, "method parameter", field.line);
                method.params.push_back(std::move(field));
                if (token_.kind != Tok::Comma) break;
                advance();
            }
        }
        expect(Tok::RParen);
        expect(Tok::Semi);
        return method;
    }

    Field parseField(bool allowDirection) {
        Field field;
        field.line = token_.line;
        if (token_.kind == Tok::Ident && (token_.text == "in" || token_.text == "out")) {
            if (!allowDirection) fail("direction marker is not allowed here");
            field.dir = token_.text == "out" ? Dir::Out : Dir::In;
            advance();
        }
        field.type = expect(Tok::Ident, "field type").text;
        field.name = expect(Tok::Ident, "field name").text;
        if (token_.kind == Tok::LBrack) {
            advance();
            field.lenRef = expect(Tok::Ident, "byte length field").text;
            expect(Tok::RBrack);
        }
        if (field.type == "bytes" && field.lenRef.empty())
            fail(field.line, "bytes field '" + field.name + "' requires a length field");
        if (field.type != "bytes" && !field.lenRef.empty())
            fail(field.line, "only bytes fields may declare a length field");
        return field;
    }

    std::string parseScopedName() {
        std::string result = expect(Tok::Ident, "identifier").text;
        while (token_.kind == Tok::Scope) {
            advance();
            result += "::" + expect(Tok::Ident, "identifier").text;
        }
        return result;
    }

    void validate(const File& file) {
        std::unordered_set<std::string> typeNames;
        for (const auto& decl : file.enums) ensureUnique(typeNames, decl.name, "type", decl.line);
        for (const auto& decl : file.structs) ensureUnique(typeNames, decl.name, "type", decl.line);

        std::unordered_set<std::string> protocolNames;
        for (const auto& decl : file.protocols)
            ensureUnique(protocolNames, decl.name, "protocol", decl.line);

        std::unordered_set<std::string> interfaceNames;
        for (const auto& iface : file.interfaces)
            ensureUnique(interfaceNames, iface.name, "interface", iface.line);

        const EnumDecl* interfaceEnum = findEnum(file, "EIPCInterface");
        if (!interfaceEnum) fail("missing EIPCInterface enum");
        std::unordered_set<std::string> interfaceMembers;
        for (const auto& member : interfaceEnum->members) interfaceMembers.insert(member.name);
        for (const auto& iface : file.interfaces) {
            if (!interfaceMembers.contains(iface.name))
                fail(iface.line, "interface '" + iface.name + "' has no EIPCInterface member");
            for (const auto& method : iface.methods) validateMethod(method);
        }

        if (file.protocols.size() != 1 || file.protocols.front().name != "IPC")
            fail("exactly one 'protocol IPC' declaration is required");
        const ProtocolDecl& protocol = file.protocols.front();
        if (!findStruct(file, protocol.requestHeader))
            fail(protocol.line, "unknown protocol request header '" + protocol.requestHeader + "'");

        const CommandDecl* interfaceCall = nullptr;
        for (const auto& command : protocol.commands) {
            validateFrame(file, command.request, "request");
            if (!command.responseNone) validateFrame(file, command.response, "response");
            if (command.name == "IPCInterfaceCall") interfaceCall = &command;
        }
        if (!interfaceCall) fail("protocol IPC must declare IPCInterfaceCall command");
        validateIPCInterfaceCall(*interfaceCall, file);
    }

    void validateIPCInterfaceCall(const CommandDecl& command, const File& file) {
        if (command.enumValue != "EIPCCommand::InterfaceCall")
            fail(command.line, "IPCInterfaceCall must use EIPCCommand::InterfaceCall");
        if (command.responseNone)
            fail(command.line, "IPCInterfaceCall response must be a frame");
        validateFrame(file, command.request, "request");
        validateFrame(file, command.response, "response");
        if (!framePayload(command.request) || !framePayload(command.response))
            fail(command.line, "IPCInterfaceCall request and response require a payload");
        if (command.request.fields.size() != 3 ||
            command.request.fields[0].name != "header" ||
            command.request.fields[1].name != "body" ||
            command.request.fields[2].name != "fencepost") {
            fail(command.line, "IPCInterfaceCall request must be: header, payload body, fencepost");
        }
        if (command.response.fields.size() != 2 ||
            command.response.fields[0].name != "header" ||
            command.response.fields[1].name != "body") {
            fail(command.line, "IPCInterfaceCall response must be: header, payload body");
        }
    }

    void validateMethod(const Method& method) {
        std::unordered_map<std::string, const Field*> prior;
        size_t inBlobs = 0;
        size_t outBlobs = 0;
        for (const auto& field : method.params) {
            if (!field.lenRef.empty()) {
                auto it = prior.find(field.lenRef);
                if (it == prior.end())
                    fail(field.line, "bytes field '" + field.name +
                                     "' references missing or later length field '" + field.lenRef + "'");
                if (!isIntegral(it->second->type))
                    fail(field.line, "bytes length field '" + field.lenRef + "' must be integral");
                size_t& count = field.dir == Dir::In ? inBlobs : outBlobs;
                if (++count > 1)
                    fail(field.line, "only one bytes field per request or response is supported");
            }
            prior[field.name] = &field;
        }
    }

    void validateFrame(const File& file, const FrameDecl& frame, const char* label) {
        for (const auto& field : frame.fields) {
            if (!field.payload && !findStruct(file, field.type) && !isIntegral(field.type))
                fail(field.line, std::string("unknown ") + label + " frame type '" + field.type + "'");
        }
    }

    static bool isIntegral(std::string_view type) {
        static const std::unordered_set<std::string_view> types = {
            "bool", "byte", "uint8", "int8", "uint16", "int16",
            "uint32", "int32", "uint64", "int64", "size_t",
        };
        return types.contains(type);
    }

    static const EnumDecl* findEnum(const File& file, std::string_view name) {
        for (const auto& decl : file.enums) if (decl.name == name) return &decl;
        return nullptr;
    }

    static const StructDecl* findStruct(const File& file, std::string_view name) {
        for (const auto& decl : file.structs) if (decl.name == name) return &decl;
        return nullptr;
    }

    static const FrameField* framePayload(const FrameDecl& frame) {
        for (const auto& field : frame.fields) if (field.payload) return &field;
        return nullptr;
    }

    void optionalSemi() {
        if (token_.kind == Tok::Semi) advance();
    }

    void advance() { token_ = lexer_.next(); }

    Token expect(Tok kind, const char* description = nullptr) {
        if (token_.kind != kind) {
            fail("expected " + std::string(description ? description : tokenName(kind)) +
                 ", got '" + token_.text + "'");
        }
        Token result = token_;
        advance();
        return result;
    }

    static const char* tokenName(Tok kind) {
        switch (kind) {
        case Tok::Ident: return "identifier";
        case Tok::Int: return "integer";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBrack: return "'['";
        case Tok::RBrack: return "']'";
        case Tok::Comma: return "','";
        case Tok::Semi: return "';'";
        case Tok::Colon: return "':'";
        case Tok::Scope: return "'::'";
        case Tok::Equal: return "'='";
        case Tok::End: return "end of file";
        }
        return "token";
    }

    void ensureUnique(std::unordered_set<std::string>& names,
                      const std::string& name,
                      const char* kind,
                      int line) {
        if (!names.insert(name).second)
            fail(line, "duplicate " + std::string(kind) + " '" + name + "'");
    }

    [[noreturn]] void fail(const std::string& message) const {
        lexer_.die(token_.line, token_.col, message);
    }

    [[noreturn]] void fail(int line, const std::string& message) const {
        lexer_.die(line, 1, message);
    }

    Lexer& lexer_;
    Token  token_;
};

class Emitter {
public:
    Emitter(std::ostream& out, const File& file) : out_(out), file_(file) {}

    void emit(const std::string& inputName, const std::string& outputName) {
        out_ << "// " << outputName << " - AUTO-GENERATED by tools/ipc_codegen.\n"
                "// Source: " << inputName << "\n"
                "// Do not edit by hand. Edit the .steamd source and rebuild.\n"
                "#pragma once\n"
                "#include \"Steam/Structs.h\"\n"
                "#include <algorithm>\n"
                "#include <cstddef>\n"
                "#include <cstring>\n"
                "#include <initializer_list>\n"
                "#include <limits>\n"
                "#include <optional>\n"
                "#include <ostream>\n"
                "#include <span>\n"
                "#include <sstream>\n"
                "#include <string>\n"
                "#include <string_view>\n"
                "#include <type_traits>\n\n";

        for (const auto& decl : file_.enums) emitEnum(decl);

        out_ << "namespace IPCMessages {\n\n";
        emitHelpers();
        emitLayouts();
        emitIPCRequest();
        emitProtocolCommandViews();
        emitIPCInterfaceCall();
        emitIPCResponse();
        out_ << "template <class T>\n"
                "std::span<const uint8> asBytes(const T& value) {\n"
                "    static_assert(std::is_trivially_copyable_v<T>);\n"
                "    return {reinterpret_cast<const uint8*>(&value), sizeof(T)};\n"
                "}\n\n";
        for (const auto& iface : file_.interfaces) emitInterface(iface);
        out_ << "} // namespace IPCMessages\n";
    }

private:
    void emitEnum(const EnumDecl& decl) {
        out_ << "enum class " << decl.name << " : " << decl.underlying << " {\n";
        for (const auto& member : decl.members)
            out_ << "    " << member.name << " = " << member.value << ",\n";
        out_ << "};\n\n"
             << "inline const char* " << decl.name << "Name(" << decl.name << " value) {\n"
                "    switch (value) {\n";
        for (const auto& member : decl.members)
            out_ << "    case " << decl.name << "::" << member.name << ": return \""
                 << member.name << "\";\n";
        out_ << "    default: return \"Unknown\";\n"
                "    }\n"
                "}\n\n"
             << "inline std::optional<" << decl.name << "> " << decl.name
             << "FromName(std::string_view name) {\n";
        for (const auto& member : decl.members)
            out_ << "    if (name == \"" << member.name << "\") return "
                 << decl.name << "::" << member.name << ";\n";
        out_ << "    return std::nullopt;\n"
                "}\n\n"
             << "inline std::ostream& operator<<(std::ostream& os, " << decl.name
             << " value) {\n"
                "    return os << " << decl.name << "Name(value);\n"
                "}\n\n";
    }

    void emitHelpers() {
        out_ << "namespace detail {\n"
                "inline std::span<uint8> BufferBytes(CUtlBuffer* buffer) {\n"
                "    if (!buffer || !buffer->Base() || buffer->m_Put < 0) return {};\n"
                "    const size_t put = static_cast<size_t>(buffer->m_Put);\n"
                "    const size_t cap = static_cast<size_t>(buffer->Capacity());\n"
                "    return {buffer->Base(), (std::min)(put, cap)};\n"
                "}\n\n"
                "template <class T>\n"
                "T Read(std::span<const uint8> bytes, size_t offset) {\n"
                "    static_assert(std::is_trivially_copyable_v<T>);\n"
                "    T value{};\n"
                "    if (offset <= bytes.size() && sizeof(T) <= bytes.size() - offset)\n"
                "        std::memcpy(&value, bytes.data() + offset, sizeof(T));\n"
                "    return value;\n"
                "}\n\n"
                "template <class T>\n"
                "void Write(std::span<uint8> bytes, size_t offset, const T& value) {\n"
                "    static_assert(std::is_trivially_copyable_v<T>);\n"
                "    if (offset <= bytes.size() && sizeof(T) <= bytes.size() - offset)\n"
                "        std::memcpy(bytes.data() + offset, &value, sizeof(T));\n"
                "}\n\n"
                "template <class T>\n"
                "size_t ByteCount(T value) {\n"
                "    static_assert(std::is_integral_v<T>);\n"
                "    if constexpr (std::is_signed_v<T>) {\n"
                "        if (value < 0) return (std::numeric_limits<size_t>::max)();\n"
                "    }\n"
                "    return static_cast<size_t>(value);\n"
                "}\n\n"
                "inline size_t Sum(std::initializer_list<size_t> values) {\n"
                "    size_t result = 0;\n"
                "    for (size_t value : values) {\n"
                "        if (value > (std::numeric_limits<size_t>::max)() - result)\n"
                "            return (std::numeric_limits<size_t>::max)();\n"
                "        result += value;\n"
                "    }\n"
                "    return result;\n"
                "}\n\n"
                "inline bool Fits(std::span<const uint8> bytes, size_t size) {\n"
                "    return size != (std::numeric_limits<size_t>::max)() && bytes.size() >= size;\n"
                "}\n\n"
                "inline std::span<uint8> Slice(std::span<uint8> bytes, size_t offset, size_t size) {\n"
                "    if (offset > bytes.size() || size > bytes.size() - offset) return {};\n"
                "    return bytes.subspan(offset, size);\n"
                "}\n\n"
                "inline std::span<const uint8> Slice(std::span<const uint8> bytes, size_t offset, size_t size) {\n"
                "    if (offset > bytes.size() || size > bytes.size() - offset) return {};\n"
                "    return bytes.subspan(offset, size);\n"
                "}\n\n"
                "inline bool CopyBytes(std::span<uint8> bytes, size_t offset, size_t capacity,\n"
                "                      std::span<const uint8> value) {\n"
                "    auto dst = Slice(bytes, offset, capacity);\n"
                "    if (dst.size() != capacity) return false;\n"
                "    const size_t count = (std::min)(dst.size(), value.size());\n"
                "    if (count) std::memcpy(dst.data(), value.data(), count);\n"
                "    if (count < dst.size()) std::memset(dst.data() + count, 0, dst.size() - count);\n"
                "    return true;\n"
                "}\n\n"
                "template <class T>\n"
                "void AppendField(std::ostringstream& os, const char* name, const T& value) {\n"
                "    os << name << '=' << value;\n"
                "}\n\n"
                "inline void AppendBytes(std::ostringstream& os, const char* name,\n"
                "                        std::span<const uint8> value) {\n"
                "    static constexpr char kHex[] = \"0123456789abcdef\";\n"
                "    auto appendHexByte = [&](unsigned byte) {\n"
                "        os << kHex[(byte >> 4) & 0x0F] << kHex[byte & 0x0F];\n"
                "    };\n"
                "    auto appendHexOffset = [&](size_t offset) {\n"
                "        bool leading = true;\n"
                "        for (int shift = static_cast<int>((sizeof(size_t) * 8) - 4); shift >= 0; shift -= 4) {\n"
                "            const size_t nibble = (offset >> shift) & 0x0F;\n"
                "            if (nibble || shift <= 12 || !leading) {\n"
                "                os << kHex[nibble];\n"
                "                leading = false;\n"
                "            }\n"
                "        }\n"
                "    };\n"
                "    os << name << '(' << value.size() << \"B)=hex[\";\n"
                "    if (!value.empty()) os << '\\n';\n"
                "    for (size_t i = 0; i < value.size(); ++i) {\n"
                "        if ((i % 16) == 0) {\n"
                "            os << \"  \";\n"
                "            appendHexOffset(i);\n"
                "            os << ':';\n"
                "        }\n"
                "        os << ' ';\n"
                "        appendHexByte(static_cast<unsigned>(value[i]));\n"
                "        if ((i % 16) == 15 && i + 1 < value.size()) os << '\\n';\n"
                "    }\n"
                "    if (!value.empty()) os << '\\n';\n"
                "    os << ']';\n"
                "}\n\n";
    }

    void emitLayouts() {
        out_ << "#pragma pack(push, 1)\n";
        for (const auto& decl : file_.structs) {
            out_ << "struct " << decl.name << "Layout {\n";
            for (const auto& field : decl.fields)
                out_ << "    " << field.type << " " << field.name << ";\n";
            out_ << "};\n\n";
        }
        out_ << "#pragma pack(pop)\n"
                "} // namespace detail\n\n";
    }

    void emitIPCRequest() {
        out_ << "class IPCRequest {\n"
                "public:\n"
                "    explicit IPCRequest(CUtlBuffer* buffer) : IPCRequest(detail::BufferBytes(buffer)) {}\n"
                "    explicit IPCRequest(std::span<uint8> bytes) : bytes_(bytes) {}\n"
                "    bool ok() const { return bytes_.size() >= sizeof(detail::IPCRequestHeaderLayout); }\n"
                "    EIPCCommand command() const { return detail::Read<EIPCCommand>(bytes_, offsetof(detail::IPCRequestHeaderLayout, command)); }\n"
                "    void set_command(EIPCCommand value) { detail::Write(bytes_, offsetof(detail::IPCRequestHeaderLayout, command), value); }\n"
                "    std::span<uint8> body() { return detail::Slice(bytes_, sizeof(detail::IPCRequestHeaderLayout), bytes_.size() >= sizeof(detail::IPCRequestHeaderLayout) ? bytes_.size() - sizeof(detail::IPCRequestHeaderLayout) : 0); }\n"
                "    std::span<const uint8> body() const { return detail::Slice(std::span<const uint8>(bytes_), sizeof(detail::IPCRequestHeaderLayout), bytes_.size() >= sizeof(detail::IPCRequestHeaderLayout) ? bytes_.size() - sizeof(detail::IPCRequestHeaderLayout) : 0); }\n"
                "    std::string DebugString() const { std::ostringstream os; os << \"IPCRequest{\"; detail::AppendField(os, \"command\", command()); os << '}'; return os.str(); }\n"
                "private:\n"
                "    std::span<uint8> bytes_;\n"
                "};\n\n";
    }

    void emitProtocolCommandViews() {
        for (const auto& protocol : file_.protocols) {
            if (protocol.name != "IPC") continue;
            for (const auto& command : protocol.commands) {
                if (command.name == "IPCInterfaceCall") continue;
                if (!canEmitFixedFrame(command.request)) continue;
                if (!command.responseNone && !canEmitFixedFrame(command.response)) continue;
                emitFixedFrameView(command.name + "Req", command.request, false);
                emitFixedFrameView(command.name + "Resp", command.response, command.responseNone);
            }
        }
    }

    void emitFixedFrameView(const std::string& className, const FrameDecl& frame, bool noneFrame) {
        out_ << "class " << className << " {\n"
                "public:\n"
             << "    explicit " << className << "(std::span<uint8> bytes) : bytes_(bytes) {}\n"
             << "    bool ok() const { return ";
        if (noneFrame)
            out_ << "bytes_.empty()";
        else
            out_ << "bytes_.size() >= " << frameFixedSizeExpr(frame);
        out_ << "; }\n";
        if (!noneFrame) {
            for (size_t i = 0; i < frame.fields.size(); ++i) {
                const FrameField& field = frame.fields[i];
                const std::string offset = frameOffsetExpr(frame, i);
                out_ << "    " << field.type << " " << field.name
                     << "() const { return detail::Read<" << field.type << ">(bytes_, " << offset << "); }\n"
                     << "    void set_" << field.name << "(" << field.type
                     << " value) { detail::Write(bytes_, " << offset << ", value); }\n";
            }
        }
        emitFrameDebugString(className, noneFrame ? FrameDecl{} : frame);
        out_ << "private:\n"
                "    std::span<uint8> bytes_;\n"
                "};\n\n";
    }

    static bool canEmitFixedFrame(const FrameDecl& frame) {
        for (const auto& field : frame.fields) {
            if (field.payload || !isIntegral(field.type)) return false;
        }
        return true;
    }

    static std::string frameFixedSizeExpr(const FrameDecl& frame) {
        std::vector<std::string> terms;
        for (const auto& field : frame.fields) terms.push_back("sizeof(" + field.type + ")");
        return sumExpr(terms);
    }

    static std::string frameOffsetExpr(const FrameDecl& frame, size_t index) {
        std::vector<std::string> terms;
        for (size_t i = 0; i < index; ++i)
            terms.push_back("sizeof(" + frame.fields[i].type + ")");
        return sumExpr(terms);
    }

    void emitFrameDebugString(const std::string& cls, const FrameDecl& frame) {
        out_ << "    std::string DebugString() const {\n"
                "        std::ostringstream os;\n"
             << "        os << \"" << cls << "{\";\n";
        for (size_t i = 0; i < frame.fields.size(); ++i) {
            if (i) out_ << "        os << ' ';\n";
            out_ << "        detail::AppendField(os, \"" << frame.fields[i].name
                 << "\", " << frame.fields[i].name << "());\n";
        }
        out_ << "        os << '}';\n"
                "        return os.str();\n"
                "    }\n";
    }

    void emitIPCInterfaceCall() {
        out_ << "class IPCInterfaceCall {\n"
                "public:\n"
                "    explicit IPCInterfaceCall(std::span<uint8> bytes) : bytes_(bytes) {}\n"
                "    bool ok() const { return bytes_.size() >= sizeof(detail::IPCInterfaceCallHeaderLayout) + sizeof(uint32); }\n"
                "    EIPCInterface interfaceID() const { return detail::Read<EIPCInterface>(bytes_, offsetof(detail::IPCInterfaceCallHeaderLayout, interfaceID)); }\n"
                "    void set_interfaceID(EIPCInterface value) { detail::Write(bytes_, offsetof(detail::IPCInterfaceCallHeaderLayout, interfaceID), value); }\n"
                "    uint32 hSteamUser() const { return detail::Read<uint32>(bytes_, offsetof(detail::IPCInterfaceCallHeaderLayout, hSteamUser)); }\n"
                "    void set_hSteamUser(uint32 value) { detail::Write(bytes_, offsetof(detail::IPCInterfaceCallHeaderLayout, hSteamUser), value); }\n"
                "    uint32 funcHash() const { return detail::Read<uint32>(bytes_, offsetof(detail::IPCInterfaceCallHeaderLayout, funcHash)); }\n"
                "    void set_funcHash(uint32 value) { detail::Write(bytes_, offsetof(detail::IPCInterfaceCallHeaderLayout, funcHash), value); }\n"
                "    uint32 fencepost() const { return detail::Read<uint32>(bytes_, bytes_.size() >= sizeof(uint32) ? bytes_.size() - sizeof(uint32) : 0); }\n"
                "    void set_fencepost(uint32 value) { if (bytes_.size() >= sizeof(uint32)) detail::Write(bytes_, bytes_.size() - sizeof(uint32), value); }\n"
                "    std::span<uint8> body() { const size_t prefix = sizeof(detail::IPCInterfaceCallHeaderLayout); const size_t suffix = sizeof(uint32); return detail::Slice(bytes_, prefix, bytes_.size() >= prefix + suffix ? bytes_.size() - prefix - suffix : 0); }\n"
                "    std::span<const uint8> body() const { const size_t prefix = sizeof(detail::IPCInterfaceCallHeaderLayout); const size_t suffix = sizeof(uint32); return detail::Slice(std::span<const uint8>(bytes_), prefix, bytes_.size() >= prefix + suffix ? bytes_.size() - prefix - suffix : 0); }\n"
                "    std::string DebugString() const { std::ostringstream os; os << \"IPCInterfaceCall{\"; detail::AppendField(os, \"interfaceID\", interfaceID()); os << ' '; detail::AppendField(os, \"hSteamUser\", hSteamUser()); os << ' '; detail::AppendField(os, \"funcHash\", funcHash()); os << ' '; detail::AppendField(os, \"fencepost\", fencepost()); os << '}'; return os.str(); }\n"
                "private:\n"
                "    std::span<uint8> bytes_;\n"
                "};\n\n";
    }

    void emitIPCResponse() {
        out_ << "class IPCResponse {\n"
                "public:\n"
                "    explicit IPCResponse(CUtlBuffer* buffer) : IPCResponse(detail::BufferBytes(buffer)) {}\n"
                "    explicit IPCResponse(std::span<uint8> bytes) : bytes_(bytes) {}\n"
                "    bool ok() const { return bytes_.size() >= sizeof(detail::IPCResponseHeaderLayout); }\n"
                "    EIPCResult result() const { return detail::Read<EIPCResult>(bytes_, offsetof(detail::IPCResponseHeaderLayout, result)); }\n"
                "    void set_result(EIPCResult value) { detail::Write(bytes_, offsetof(detail::IPCResponseHeaderLayout, result), value); }\n"
                "    std::span<uint8> body() { return detail::Slice(bytes_, sizeof(detail::IPCResponseHeaderLayout), bytes_.size() >= sizeof(detail::IPCResponseHeaderLayout) ? bytes_.size() - sizeof(detail::IPCResponseHeaderLayout) : 0); }\n"
                "    std::span<const uint8> body() const { return detail::Slice(std::span<const uint8>(bytes_), sizeof(detail::IPCResponseHeaderLayout), bytes_.size() >= sizeof(detail::IPCResponseHeaderLayout) ? bytes_.size() - sizeof(detail::IPCResponseHeaderLayout) : 0); }\n"
                "    std::string DebugString() const { std::ostringstream os; os << \"IPCResponse{\"; detail::AppendField(os, \"result\", result()); os << '}'; return os.str(); }\n"
                "private:\n"
                "    std::span<uint8> bytes_;\n"
                "};\n\n";
    }

    void emitInterface(const Interface& iface) {
        out_ << "namespace " << iface.name << " {\n"
             << "inline constexpr EIPCInterface kInterface = EIPCInterface::" << iface.name << ";\n\n";
        for (const auto& method : iface.methods) emitReq(iface, method);
        for (const auto& method : iface.methods) emitResp(method);
        out_ << "} // namespace " << iface.name << "\n\n";
    }

    void emitReq(const Interface& iface, const Method& method) {
        const std::vector<Field> fields = fieldsFor(method, Dir::In, false);
        const std::string cls = method.name + "Req";
        out_ << "class " << cls << " {\n"
                "public:\n"
             << "    explicit " << cls << "(CUtlBuffer* buffer) : " << cls
             << "(detail::BufferBytes(buffer)) {}\n"
             << "    explicit " << cls << "(std::span<uint8> bytes) : request_(bytes), call_(request_.body()) {}\n"
                "    bool ok() const { return request_.ok() && request_.command() == EIPCCommand::InterfaceCall && call_.ok() && call_.interfaceID() == kInterface && detail::Fits(call_.body(), bodySize()); }\n"
                "    uint32 fencepost() const { return call_.fencepost(); }\n"
                "    void set_fencepost(uint32 value) { call_.set_fencepost(value); }\n";
        emitFieldAccessors(fields, fields, {});
        emitDebugString(cls, fields);
        out_ << "private:\n"
             << "    size_t bodySize() const { return " << sumExpr(fields, fields, {}, false) << "; }\n"
                "    std::span<uint8> body() { return call_.body(); }\n"
                "    std::span<const uint8> body() const { return call_.body(); }\n"
                "    IPCRequest request_;\n"
                "    IPCInterfaceCall call_;\n"
                "};\n\n";
    }

    void emitResp(const Method& method) {
        std::vector<Field> fields = fieldsFor(method, Dir::Out, true);
        const std::vector<std::string> external = externalRefs(method, fields);
        const std::string cls = method.name + "Resp";
        out_ << "class " << cls << " {\n"
                "public:\n"
             << "    explicit " << cls << "(CUtlBuffer* buffer";
        emitCtorParams(external);
        out_ << ") : " << cls << "(detail::BufferBytes(buffer)";
        emitCtorArgs(external);
        out_ << ") {}\n"
             << "    explicit " << cls << "(std::span<uint8> bytes";
        emitCtorParams(external);
        out_ << ") : response_(bytes)";
        for (const auto& name : external) out_ << ", " << name << "_(" << name << ")";
        out_ << " {}\n"
                "    bool ok() const { return response_.ok() && detail::Fits(response_.body(), minimumBodySize()); }\n"
                "    EIPCResult result() const { return response_.result(); }\n"
                "    void set_result(EIPCResult value) { response_.set_result(value); }\n";
        emitFieldAccessors(fields, fields, external);
        emitDebugString(cls, fields);
        out_ << "private:\n"
             << "    size_t minimumBodySize() const { return "
             << sumExpr(fields, fields, external, true) << "; }\n"
                "    std::span<uint8> body() { return response_.body(); }\n"
                "    std::span<const uint8> body() const { return response_.body(); }\n"
                "    IPCResponse response_;\n";
        for (const auto& name : external) out_ << "    size_t " << name << "_;\n";
        out_ << "};\n\n";
    }

    void emitFieldAccessors(const std::vector<Field>& fields,
                            const std::vector<Field>& sideFields,
                            const std::vector<std::string>& external) {
        for (size_t i = 0; i < fields.size(); ++i) {
            const Field& field = fields[i];
            const std::string offset = offsetExpr(fields, i, sideFields, external);
            if (field.type == "bytes") {
                const std::string length = lengthExpr(field, sideFields, external);
                out_ << "    std::span<uint8> " << field.name << "() { return detail::Slice(body(), "
                     << offset << ", " << length << "); }\n"
                     << "    std::span<const uint8> " << field.name
                     << "() const { return detail::Slice(body(), " << offset << ", " << length << "); }\n"
                     << "    bool set_" << field.name
                     << "(std::span<const uint8> value) { return detail::CopyBytes(body(), "
                     << offset << ", " << length << ", value); }\n";
            } else {
                out_ << "    " << field.type << " " << field.name
                     << "() const { return detail::Read<" << field.type << ">(body(), " << offset << "); }\n"
                     << "    void set_" << field.name << "(" << field.type
                     << " value) { detail::Write(body(), " << offset << ", value); }\n";
            }
        }
    }

    void emitDebugString(const std::string& cls, const std::vector<Field>& fields) {
        out_ << "    std::string DebugString() const {\n"
                "        std::ostringstream os;\n"
             << "        os << \"" << cls << "{\";\n";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) out_ << "        os << ' ';\n";
            if (fields[i].type == "bytes")
                out_ << "        detail::AppendBytes(os, \"" << fields[i].name
                     << "\", " << fields[i].name << "());\n";
            else
                out_ << "        detail::AppendField(os, \"" << fields[i].name
                     << "\", " << fields[i].name << "());\n";
        }
        out_ << "        os << '}';\n"
                "        return os.str();\n"
                "    }\n";
    }

    static std::vector<Field> fieldsFor(const Method& method, Dir direction, bool includeReturn) {
        std::vector<Field> fields;
        if (includeReturn && method.retType != "void") {
            Field result;
            result.type = method.retType;
            result.name = "returnValue";
            fields.push_back(result);
        }
        for (const auto& field : method.params)
            if (field.dir == direction) fields.push_back(field);
        return fields;
    }

    static std::vector<std::string> externalRefs(const Method& method,
                                                 const std::vector<Field>& fields) {
        std::vector<std::string> result;
        for (const auto& field : fields) {
            if (field.lenRef.empty() || containsField(fields, field.lenRef)) continue;
            if (std::find(result.begin(), result.end(), field.lenRef) == result.end())
                result.push_back(field.lenRef);
        }
        return result;
    }

    static bool containsField(const std::vector<Field>& fields, std::string_view name) {
        return std::any_of(fields.begin(), fields.end(),
                           [name](const Field& field) { return field.name == name; });
    }

    static bool containsName(const std::vector<std::string>& names, std::string_view name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    static bool isIntegral(std::string_view type) {
        static const std::unordered_set<std::string_view> types = {
            "bool", "byte", "uint8", "int8", "uint16", "int16",
            "uint32", "int32", "uint64", "int64", "size_t",
        };
        return types.contains(type);
    }

    static std::string lengthExpr(const Field& field,
                                  const std::vector<Field>& sideFields,
                                  const std::vector<std::string>& external) {
        if (containsField(sideFields, field.lenRef))
            return "detail::ByteCount(" + field.lenRef + "())";
        if (containsName(external, field.lenRef)) return field.lenRef + "_";
        return "0";
    }

    static std::string offsetExpr(const std::vector<Field>& fields,
                                  size_t index,
                                  const std::vector<Field>& sideFields,
                                  const std::vector<std::string>& external) {
        std::vector<std::string> terms;
        for (size_t i = 0; i < index; ++i) {
            if (fields[i].type == "bytes")
                terms.push_back(lengthExpr(fields[i], sideFields, external));
            else
                terms.push_back("sizeof(" + fields[i].type + ")");
        }
        return sumExpr(terms);
    }

    static std::string sumExpr(const std::vector<Field>& fields,
                               const std::vector<Field>& sideFields,
                               const std::vector<std::string>& external,
                               bool minimum) {
        std::vector<std::string> terms;
        for (const auto& field : fields) {
            if (field.type != "bytes") {
                terms.push_back("sizeof(" + field.type + ")");
            } else if (!minimum || !containsField(sideFields, field.lenRef)) {
                terms.push_back(lengthExpr(field, sideFields, external));
            }
        }
        return sumExpr(terms);
    }

    static std::string sumExpr(const std::vector<std::string>& terms) {
        if (terms.empty()) return "0";
        std::string result = "detail::Sum({";
        for (size_t i = 0; i < terms.size(); ++i) {
            if (i) result += ", ";
            result += terms[i];
        }
        return result + "})";
    }

    void emitCtorParams(const std::vector<std::string>& external) {
        for (const auto& name : external) out_ << ", size_t " << name;
    }

    void emitCtorArgs(const std::vector<std::string>& external) {
        for (const auto& name : external) out_ << ", " << name;
    }

    std::ostream& out_;
    const File&   file_;
};

namespace {

constexpr const char* kVersion = "ipc_codegen 1.0.0";

void printUsage(std::FILE* out) {
    std::fprintf(out,
        "usage: ipc_codegen [options] <input.steamd>...\n"
        "  --cpp_out=DIR   write generated <name>.gen.h into DIR (default: .)\n"
        "  --version       print version and exit\n"
        "  -h, --help      print this help and exit\n");
}

std::string stem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = path.find_last_of('.');
    const size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
    return path.substr(start, end - start);
}

bool generate(const std::string& inputPath, const std::string& cppOut) {
    std::ifstream input(inputPath, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "ipc_codegen: cannot open %s\n", inputPath.c_str());
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    Lexer lexer(source, inputPath);
    Parser parser(lexer);
    File file = parser.parse();

    const std::string header = stem(inputPath) + ".gen.h";
    const std::string outputPath = cppOut.empty() ? header : cppOut + "/" + header;

    std::ostringstream generated;
    Emitter emitter(generated, file);
    emitter.emit(inputPath, header);

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "ipc_codegen: cannot write %s\n", outputPath.c_str());
        return false;
    }
    output << generated.str();
    if (!output) {
        std::fprintf(stderr, "ipc_codegen: failed to write %s\n", outputPath.c_str());
        return false;
    }

    std::fprintf(stderr, "ipc_codegen: wrote %s (%zu interfaces)\n",
                 outputPath.c_str(), file.interfaces.size());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string              cppOut = ".";
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--version") {
            std::printf("%s\n", kVersion);
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            printUsage(stdout);
            return 0;
        }
        if (arg.rfind("--cpp_out=", 0) == 0) {
            cppOut = std::string(arg.substr(std::string_view("--cpp_out=").size()));
            continue;
        }
        if (arg == "--cpp_out") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ipc_codegen: --cpp_out requires a directory\n");
                return 1;
            }
            cppOut = argv[++i];
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            std::fprintf(stderr, "ipc_codegen: unknown option '%s'\n", argv[i]);
            printUsage(stderr);
            return 1;
        }
        inputs.emplace_back(arg);
    }

    if (inputs.empty()) {
        printUsage(stderr);
        return 1;
    }

    for (const std::string& input : inputs) {
        if (!generate(input, cppOut)) return 1;
    }
    return 0;
}
