#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <rtc/rtc.hpp>
#include <rtc/datachannel.hpp>
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstddef>
#include <GLFW/glfw3.h>
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>
#include <Windows.h>
#include <ShlObj.h>
#include "hashing.h"
#include "../Dependancy/imgui/imgui.h"
#include "../Dependancy/imgui/backends/imgui_impl_glfw.h"
#include "../Dependancy/imgui/backends/imgui_impl_opengl3.h"

namespace fs = std::filesystem;

std::deque<std::string> log_buffer;
std::mutex log_mutex;

// 전역 변수
std::function<void()> clipboard_task;
std::mutex clipboard_mutex;

void run_on_main_thread(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(clipboard_mutex);
    clipboard_task = task;
}


void add_log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    log_buffer.push_back(msg);
    if (log_buffer.size() > 1000) log_buffer.pop_front(); // 오래된 로그 제거
}

bool encode_string_to_image_to_memory(const std::string& input, std::vector<unsigned char>& out_png_data) {
    const int width = 64, height = 64;
    std::vector<unsigned char> pixels(width * height * 4, 0);

    for (size_t i = 0; i < input.size(); ++i) {
        size_t pixel_index = i * 4;
        pixels[pixel_index + 0] = input[i];   // R = 문자
        pixels[pixel_index + 1] = 0;          // G
        pixels[pixel_index + 2] = 0;          // B
        pixels[pixel_index + 3] = 255;        // A (불투명)
    }

    out_png_data = pixels;

    return !out_png_data.empty();
}

std::string decode_string_from_image_memory(std::vector<unsigned char> image_data) {
    const int width = 64, height = 64;
    unsigned char* data = image_data.data();

    std::string result;
    size_t total = width * height;
    for (size_t i = 0; i < total; ++i) {
        unsigned char r = data[i * 4];
        if (r == 0) break; // padding or null
        result += (char)r;
    }

    return result;
}

GLuint create_texture_from_png_memory(const unsigned char* data, int width, int height) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

bool copy_image_to_clipboard() {
    fs::path absPath = fs::current_path() / "tmp.png";
    std::wstring file_path = absPath.wstring();
    size_t path_len = file_path.length() + 1; // +1 for null terminator
    size_t buffer_size = sizeof(DROPFILES) + path_len * sizeof(wchar_t) + sizeof(wchar_t); // double null

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, buffer_size);
    if (!hGlobal) return false;

    DROPFILES* df = static_cast<DROPFILES*>(GlobalLock(hGlobal));
    if (!df) {
        GlobalFree(hGlobal);
        return false;
    }

    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE; // Unicode path
    wchar_t* data = reinterpret_cast<wchar_t*>((BYTE*)df + sizeof(DROPFILES));
    wmemset(data, 0, path_len + 1); // double null
    wcsncpy_s(data, path_len, file_path.c_str(), path_len);

    GlobalUnlock(hGlobal);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(hGlobal);
        return false;
    }

    EmptyClipboard();
    SetClipboardData(CF_HDROP, hGlobal); // clipboard takes ownership
    CloseClipboard();

    return true;
}


bool get_image_from_clipboard(std::vector<unsigned char>& out_rgba, int& width, int& height) {
    fs::path absPath = fs::current_path() / "tmp.png";
    std::string file_path = absPath.string();

    if (!OpenClipboard(nullptr)) {
        add_log(u8"클립보드 열기 실패");
        return false;
    }

    HANDLE hData = GetClipboardData(CF_DIB);
    if (!hData) {
        add_log(u8"클립보드에 CF_DIB 포맷 없음");
        CloseClipboard();
        return false;
    }

    void* pData = GlobalLock(hData);
    if (!pData) {
        add_log(u8"CF_DIB 잠금 실패");
        CloseClipboard();
        return false;
    }

    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)pData;
    width = bih->biWidth;
    height = abs(bih->biHeight);
    int channels = bih->biBitCount / 8;

    if (channels != 4) {
        add_log(u8"지원하지 않는 비트수: " + std::to_string(bih->biBitCount));
        GlobalUnlock(hData);
        CloseClipboard();
        return false;
    }

    const unsigned char* src = (unsigned char*)(bih + 1); // 픽셀 데이터 시작
    std::vector<unsigned char> rgba(width * height * 4);

    for (int y = 0; y < height; ++y) {
        int src_y = bih->biHeight > 0 ? (height - 1 - y) : y;
        for (int x = 0; x < width; ++x) {
            const unsigned char* pixel = src + (src_y * width + x) * 4;
            unsigned char* dst = &rgba[(y * width + x) * 4];
            dst[0] = pixel[2]; // R
            dst[1] = pixel[1]; // G
            dst[2] = pixel[0]; // B
            dst[3] = pixel[3]; // A
        }
    }

    GlobalUnlock(hData);
    CloseClipboard();

    out_rgba = rgba;
    return true;
}


GLuint create_texture_from_rgba_memory(const unsigned char* data, int width, int height) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}


GLuint offer_texture = 0;
std::vector<unsigned char> clipboard_data;

void send(std::string& path) {
    fs::path absPath = fs::current_path() / "Upload" / path;
    add_log(absPath.string());

    auto file = std::make_shared<std::ifstream>(absPath, std::ios::binary);
    if (!*file) {
        add_log(u8"파일 열기 실패!");
        return;
    }

    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");

    auto pc = std::make_shared<rtc::PeerConnection>(config);

    pc->onStateChange([](rtc::PeerConnection::State state) {
        add_log(u8"[send] Connection state: " + static_cast<int>(state));
        if (state == rtc::PeerConnection::State::Connected) {
            add_log(u8"연결됨!");
        }
        else if (state == rtc::PeerConnection::State::Failed) {
            add_log(u8"[send] 연결 실패!");
        }
    });

    pc->onLocalCandidate([](const rtc::Candidate& cand) {
        add_log(u8"Local Candidate: " + std::string(cand));
    });

    auto dc = pc->createDataChannel("file");

    dc->onOpen([dc, file, absPath]() {
        add_log(u8"전송 시작...");
        dc->send(absPath.filename().string());

        const size_t chunkSize = 16384;
        std::vector<std::byte> buffer(chunkSize);

        while (file && !file->eof()) {
            file->read(reinterpret_cast<char*>(buffer.data()), chunkSize);
            std::streamsize readBytes = file->gcount();
            if (readBytes > 0) {
                dc->send(rtc::binary(buffer.data(), buffer.data() + readBytes));
            }
        }

        dc->send("__EOF__"); // 전송 완료 표시
        add_log(u8"전송 완료!\n창을 닫아도 좋습니다!");
    });

    dc->onMessage([](std::variant<rtc::binary, std::string> data) {
        if (std::holds_alternative<std::string>(data)) {
            add_log(u8"[send] 받은 메시지: " + std::get<std::string>(data));
        }
    });

    pc->onGatheringStateChange([pc](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            auto desc = pc->localDescription();
            if (!desc) return;

            std::string sdp = std::string(*desc);
            std::string encoded = compress(sdp);
            encoded.erase(std::remove(encoded.begin(), encoded.end(), '\n'), encoded.end());

            add_log(u8"=== SDP Offer (클립보드에 복사됨!) ===");
            add_log(encoded);
            add_log(u8"===========================");

            std::vector<unsigned char> png_data;
            encode_string_to_image_to_memory(encoded, png_data);
            offer_texture = create_texture_from_png_memory(png_data.data(), 64, 64);
            stbi_write_png("tmp.png", 64, 64, 4, png_data.data(), 64*4);
            copy_image_to_clipboard();

            add_log(u8"[send] Answer 입력(붙여넣기!):");
            std::string encodedAnswer;
            while(clipboard_data.empty()) std::this_thread::sleep_for(std::chrono::seconds(1));
            encodedAnswer = decode_string_from_image_memory(clipboard_data);
            std::string decodedAnswer = decompress(encodedAnswer);

            rtc::Description answer(decodedAnswer, rtc::Description::Type::Answer);
            pc->setRemoteDescription(answer);

            add_log(u8"[send] 기다리는 중...");
        }
    });

    pc->setLocalDescription();

    add_log(u8"[send] 기다리는 중...");
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

GLuint answer_texture = 0;

void recieve() {
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");

    auto pc = std::make_shared<rtc::PeerConnection>(config);

    pc->onStateChange([](rtc::PeerConnection::State state) {
        add_log(u8"[recv] Connection state: " + static_cast<int>(state));
        if (state == rtc::PeerConnection::State::Connected) {
            add_log(u8"연결됨!");
        }
        else if (state == rtc::PeerConnection::State::Failed) {
            add_log(u8"[recv] 연결 실패!");
        }
    });

    pc->onLocalCandidate([](const rtc::Candidate& cand) {
        add_log(u8"Local Candidate: " + std::string(cand));
    });

    pc->onGatheringStateChange([pc](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            auto desc = pc->localDescription();
            if (!desc) return;

            std::string sdp = std::string(*desc);
            std::istringstream iss(sdp);
            std::ostringstream oss;
            std::string line;
            while (std::getline(iss, line)) {
                if (line.rfind("a=setup:actpass", 0) == 0) {
                    oss << "a=setup:active\n";
                }
                else {
                    oss << line << "\n";
                }
            }

            std::string encoded = compress(oss.str());
            encoded.erase(std::remove(encoded.begin(), encoded.end(), '\n'), encoded.end());

            add_log(u8"=== SDP Answer (복사하기!) ===");
            add_log(encoded);
            add_log(u8"===========================");

            std::vector<unsigned char> png_data;
            encode_string_to_image_to_memory(encoded, png_data);
            answer_texture = create_texture_from_png_memory(png_data.data(), 64, 64);
            stbi_write_png("tmp.png", 64, 64, 4, png_data.data(), 64 * 4);
            copy_image_to_clipboard();
        }
    });

    pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
        auto outFile = std::make_shared<std::ofstream>();
        dc->onMessage([dc, outFile](std::variant<rtc::binary, std::string> data) mutable {
            if (std::holds_alternative<std::string>(data)) {
                auto str = std::get<std::string>(data);
                if (str == "__EOF__") {
                    add_log(u8"파일 수신 완료!\n창을 닫아도 좋습니다!");
                    if (outFile->is_open()) outFile->close();
                }
                else {
                    fs::path absPath = fs::current_path() / "Download";
                    fs::create_directories(absPath);
                    absPath /= str;
                    outFile->open(absPath, std::ios::binary);
                    if (!*outFile) {
                        add_log(u8"파일 저장 실패!");
                    }
                    else {
                        add_log(u8"파일 저장 시작: " + str);
                    }
                }
            }
            else if (std::holds_alternative<rtc::binary>(data)) {
                const auto& bin = std::get<rtc::binary>(data);
                if (outFile->is_open()) {
                    outFile->write(reinterpret_cast<const char*>(bin.data()), bin.size());
                }
            }
        });
    });

    add_log(u8"[recv] Offer 입력(붙여넣기!):");
    while (clipboard_data.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        //add_log(std::to_string(clipboard_data.size()));
    }

    std::string encodedOffer = decode_string_from_image_memory(clipboard_data);
    std::string decodedOffer = decompress(encodedOffer);
    rtc::Description offer(decodedOffer, rtc::Description::Type::Offer);
    pc->setRemoteDescription(offer);

    pc->setLocalDescription();

    add_log(u8"[recv] 기다리는 중...");
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void draw_log_window() {
    ImGui::Begin("Log");

    std::lock_guard<std::mutex> lock(log_mutex);
    for (const auto& line : log_buffer) {
        ImGui::TextUnformatted(line.c_str());
    }

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f); // 자동 스크롤

    ImGui::End();
}


GLuint clipboard_texture = 0;
int clipboard_width, clipboard_height;
static int mode = 0;

void draw_webrtc_ui() {
    ImGui::Begin("File Transfer System");

    ImGui::RadioButton("Send", &mode, 0); ImGui::SameLine();
    ImGui::RadioButton("Receive", &mode, 1);

    if (ImGui::IsKeyReleased(ImGuiKey_V) && ImGui::GetIO().KeyCtrl) {
        add_log("loading from clipboard");
        int width = 0, height = 0;
        if (get_image_from_clipboard(clipboard_data, width, height)) {
            clipboard_texture = create_texture_from_rgba_memory(clipboard_data.data(), width, height);
            clipboard_width = width;
            clipboard_height = height;
        }
    }

    if (mode == 0) {
        static char filePath[256] = "";
        static std::string generatedOffer;

        ImGui::InputText("File Path", filePath, sizeof(filePath));
        if (ImGui::Button("Host")) {
            std::string path = filePath;
            std::thread([](std::string p) {
                send(p);
            }, path).detach();
        }

        if (offer_texture != 0) {
            ImGui::Text("Encoded PNG Image:");
            ImGui::Image((ImTextureID)(intptr_t)offer_texture, ImVec2(64, 64));
        }

        if (clipboard_texture != 0) {
            ImGui::Text("Answer Image:");
            ImGui::Image((ImTextureID)(intptr_t)clipboard_texture, ImVec2(clipboard_width, clipboard_height));
        }
    }
    else {
        if (ImGui::Button("Start")) {
            std::thread([]() {
                recieve();
            }).detach();
        }

        if (clipboard_texture != 0) {
            ImGui::Text("Offer Image (from Clipboard):");
            ImGui::Image((ImTextureID)(intptr_t)clipboard_texture, ImVec2(clipboard_width, clipboard_height));
        }

        if (answer_texture != 0) {
            ImGui::Text("Generated Answer Image:");
            ImGui::Image((ImTextureID)(intptr_t)answer_texture, ImVec2(64, 64));
        }
    }

    ImGui::End();
}

std::vector<const char*> send_tutorial = {
    u8"전송시 사용법!",
    u8"1. Upload 폴더 내에 있는 파일명을 넣는다.",
    u8"2. Host 버튼을 누른다.",
    u8"3. 상대 DM에 Ctrl+V로 붙여넣는다.",
    u8"4. 상대가 보낸 사진을 복사하여 붙여넣는다.",
    u8"5. 전송이 완료되었다는 메시지가 뜨면 종료한다."
};

std::vector<const char*> recieve_tutorial = {
    u8"받기시 사용법!",
    u8"1. Start 버튼을 누른다.",
    u8"2. 상대가 보낸 사진을 복사하여 붙여넣는다.",
    u8"3. 두번째 사진이 나오면 상대에게 Ctrl+V로 붙여넣는다.",
    u8"4. 전송이 완료되었다는 메시지가 뜨면 Download 폴더에",
    u8"파일이 있는지 확인한다."
};

void draw_tutorial_ui() {
    ImGui::Begin(u8"사용법");

    if (mode == 0) {
        for (const char* line : send_tutorial) {
            ImGui::TextUnformatted(line);
        }
    }
    else {
        for (const char* line : recieve_tutorial) {
            ImGui::TextUnformatted(line);
        }
    }

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f); // 자동 스크롤

    ImGui::End();
}

int main() {
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 130";
    GLFWwindow* window = glfwCreateWindow(800, 600, "FTS", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    io.Fonts->AddFontDefault(); // 기본 폰트 (영문)
    ImFont* font_korean = io.Fonts->AddFontFromFileTTF("font.ttf", 13.0f, nullptr, io.Fonts->GetGlyphRangesKorean());
    io.FontDefault = font_korean; // << 이거 추가!
    io.Fonts->Build();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        std::lock_guard<std::mutex> lock(clipboard_mutex);
        if (clipboard_task) {
            clipboard_task();
            clipboard_task = nullptr;
        }

        draw_webrtc_ui();
        draw_log_window();
        draw_tutorial_ui();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}