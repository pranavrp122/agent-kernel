#include <iostream>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "kernel.hpp"

int main() {
#ifdef _WIN32
    // binary mode stops the windows runtime from rewriting every newline as crlf
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    ak::Kernel kernel;
    std::string line;
    // one json object per line in, one json object per line out, flushed so the pipe never stalls
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const nlohmann::json req = nlohmann::json::parse(line, nullptr, false);
        nlohmann::json res;
        if (req.is_discarded() || !req.is_object()) res = nlohmann::json{{"ok", false}, {"error", "bad_json"}};
        else res = kernel.handle(req);
        std::cout << res.dump() << '\n' << std::flush;
    }
    return 0;
}
