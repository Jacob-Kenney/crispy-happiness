#include "llama.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

template <typename Function>
Function symbol(void * library, const char * name) {
    void * address = dlsym(library, name);
    if (!address) throw std::runtime_error(std::string("Missing symbol: ") + name);
    return reinterpret_cast<Function>(address);
}

#define LOAD(library, name) const auto name##_fn = symbol<decltype(&name)>(library, #name)

constexpr std::array<std::string_view, 7> actions{"wait", "left", "right", "up", "down", "a", "b"};
constexpr std::array<double, 7> bias{-0.096, -4.651, 0.138, 0.194, 0.202, 0.209, 0.135};

int action_index(std::string word) {
    auto start = word.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) return -1;
    word = word.substr(start, word.find_last_not_of(" \t\n\r\f\v") - start + 1);
    for (char & ch : word) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (word == "none" || word == "noop" || word == "stay") return 0;
    if (word == "jump") return 5;
    if (word == "save") return 6;
    auto found = std::find(actions.begin(), actions.end(), word);
    return found == actions.end() ? -1 : static_cast<int>(found - actions.begin());
}

std::string make_prompt(const std::string & observation) {
    return "<|im_start|>system\n"
           "You pilot a ship up a narrow canyon in Canyon Racer.\n"
           "Walls, rocks and items scroll toward you.\n"
           "Reply with exactly one action word:\n"
           "left, right, up, down, a (jump), b (save), wait (release buttons)\n"
           "Hold a to jump higher; while airborne you cannot crash.\n"
           "Other words are invalid and penalized.<|im_end|>\n"
           "<|im_start|>user\nState:\n" + observation +
           "\nReply with one word: left, right, up, down, a, b, wait.<|im_end|>\n"
           "<|im_start|>assistant\n";
}

void respond(int socket, std::string_view status, const std::string & body) {
    std::string response = "HTTP/1.1 " + std::string(status) +
        "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string_view remaining(response);
    while (!remaining.empty()) {
        auto sent = send(socket, remaining.data(), remaining.size(), MSG_NOSIGNAL);
        if (sent <= 0) return;
        remaining.remove_prefix(sent);
    }
}

int main(int argc, char ** argv) {
    if (argc != 2) {
        std::cerr << "Usage: jetson-policy-server MODEL.gguf\n";
        return 2;
    }
    void * ggml = dlopen("libggml.so", RTLD_NOW | RTLD_GLOBAL);
    void * library = dlopen("libllama.so", RTLD_NOW | RTLD_LOCAL);
    if (!ggml || !library) {
        std::cerr << dlerror() << '\n';
        return 1;
    }
    LOAD(ggml, ggml_backend_load_all_from_path);
    LOAD(ggml, ggml_backend_dev_by_type);
    LOAD(library, llama_backend_init);
    LOAD(library, llama_model_default_params);
    LOAD(library, llama_model_load_from_file);
    LOAD(library, llama_model_free);
    LOAD(library, llama_model_get_vocab);
    LOAD(library, llama_vocab_n_tokens);
    LOAD(library, llama_token_to_piece);
    LOAD(library, llama_context_default_params);
    LOAD(library, llama_init_from_model);
    LOAD(library, llama_free);
    LOAD(library, llama_tokenize);
    LOAD(library, llama_get_memory);
    LOAD(library, llama_memory_clear);
    LOAD(library, llama_batch_get_one);
    LOAD(library, llama_decode);
    LOAD(library, llama_get_logits_ith);

    ggml_backend_load_all_from_path_fn("/canyon/runtime-parallel");
    llama_backend_init_fn();
    auto gpu = ggml_backend_dev_by_type_fn(GGML_BACKEND_DEVICE_TYPE_IGPU);
    if (!gpu) gpu = ggml_backend_dev_by_type_fn(GGML_BACKEND_DEVICE_TYPE_GPU);
    if (!gpu) throw std::runtime_error("CUDA backend unavailable");
    ggml_backend_dev_t devices[]{gpu, nullptr};
    auto model_params = llama_model_default_params_fn();
    model_params.devices = devices;
    model_params.n_gpu_layers = 99;
    std::unique_ptr<llama_model, decltype(llama_model_free_fn)> model(
        llama_model_load_from_file_fn(argv[1], model_params), llama_model_free_fn);
    if (!model) throw std::runtime_error("Could not load Canyon Racer model");
    auto params = llama_context_default_params_fn();
    params.n_ctx = 512;
    params.n_batch = 512;
    params.n_ubatch = 512;
    params.n_threads = 4;
    params.n_threads_batch = 4;
    params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    std::unique_ptr<llama_context, decltype(llama_free_fn)> context(
        llama_init_from_model_fn(model.get(), params), llama_free_fn);
    if (!context) throw std::runtime_error("Could not initialize Canyon Racer context");

    auto vocab = llama_model_get_vocab_fn(model.get());
    std::vector<std::pair<int, int>> action_tokens;
    for (int id = 0; id < llama_vocab_n_tokens_fn(vocab); ++id) {
        char piece[256];
        int len = llama_token_to_piece_fn(vocab, id, piece, sizeof(piece), 0, false);
        if (len <= 0) continue;
        int group = action_index(std::string(piece, len));
        if (group >= 0) action_tokens.emplace_back(id, group);
    }
    if (action_tokens.empty()) throw std::runtime_error("No action tokens in vocabulary");

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error(std::strerror(errno));
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8080);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener, 8)) {
        throw std::runtime_error(std::strerror(errno));
    }
    std::cout << "Canyon Racer listening on 127.0.0.1:8080 with "
              << action_tokens.size() << " action tokens\n" << std::flush;

    for (;;) {
        int client = accept(listener, nullptr, nullptr);
        if (client < 0) continue;
        const auto close_client = [](int * fd) { close(*fd); };
        std::unique_ptr<int, decltype(close_client)> guard(&client, close_client);
        timeval timeout{3, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        std::string request;
        char buffer[2048];
        auto read_more = [&] {
            auto length = recv(client, buffer, sizeof(buffer), 0);
            if (length <= 0) return false;
            request.append(buffer, length);
            return true;
        };
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192 && read_more()) {}
        auto header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;
        if (request.starts_with("GET /health ")) {
            respond(client, "200 OK", "{\"status\":\"ok\"}");
            continue;
        }
        if (!request.starts_with("POST /action ")) {
            respond(client, "404 Not Found", "{\"error\":\"unknown endpoint\"}");
            continue;
        }
        std::istringstream headers(request.substr(0, header_end));
        std::string line;
        int length = -1;
        while (std::getline(headers, line)) {
            auto name_end = line.find(':');
            auto name = line.substr(0, name_end);
            for (char & ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
            if (name == "content-length" && name_end != std::string::npos) {
                std::istringstream value(line.substr(name_end + 1));
                value >> length;
                if (value.fail()) length = -1;
            }
        }
        size_t body_start = header_end + 4;
        if (length < 1 || length > 2048) {
            respond(client, "400 Bad Request", "{\"error\":\"expected 1 to 2048 bytes of game state\"}");
            continue;
        }
        while (request.size() < body_start + length && read_more()) {}
        if (request.size() < body_start + length) continue;
        auto input = request.substr(body_start, length);
        while (!input.empty() && (input.back() == '\n' || input.back() == '\r')) input.pop_back();
        auto last = input.rfind("\nlast=");
        if (last == std::string::npos || action_index(input.substr(last + 6)) < 0) {
            respond(client, "400 Bad Request", "{\"error\":\"end the game state with last=<action>\"}");
            continue;
        }

        auto start = std::chrono::steady_clock::now();
        auto prompt = make_prompt(input);
        std::array<llama_token, 512> tokens;
        int count = llama_tokenize_fn(vocab, prompt.data(), prompt.size(), tokens.data(), tokens.size(), false, true);
        if (count <= 0) {
            respond(client, "400 Bad Request", "{\"error\":\"prompt exceeds context\"}");
            continue;
        }
        llama_memory_clear_fn(llama_get_memory_fn(context.get()), false);
        if (llama_decode_fn(context.get(), llama_batch_get_one_fn(tokens.data(), count)) != 0) {
            respond(client, "500 Internal Server Error", "{\"error\":\"CUDA inference failed\"}");
            continue;
        }
        const float * logits = llama_get_logits_ith_fn(context.get(), -1);
        if (!logits) {
            respond(client, "500 Internal Server Error", "{\"error\":\"missing action logits\"}");
            continue;
        }
        float max_logit = action_tokens.empty() ? 0 : logits[action_tokens.front().first];
        for (auto [id, group] : action_tokens) max_logit = std::max(max_logit, logits[id]);
        std::array<double, actions.size()> scores{};
        for (auto [id, group] : action_tokens) scores[group] += std::exp(double(logits[id] - max_logit));
        for (size_t i = 0; i < scores.size(); ++i) scores[i] = std::log(scores[i]) + bias[i];
        auto selected = std::max_element(scores.begin(), scores.end()) - scores.begin();
        auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
        respond(client, "200 OK", "{\"action\":\"" + std::string(actions[selected]) +
                "\",\"prompt_tokens\":" + std::to_string(count) +
                ",\"inference_ms\":" + std::to_string(elapsed.count()) + "}");
    }
}
