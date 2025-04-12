#include <iostream>
#include <string>
#include <vector>
#include <sstream>

static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64 ���ڵ� �Լ�
std::string base64_encode(const std::string& input) {
    std::string result;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) result.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');
    return result;
}

// Base64 ���ڵ� �Լ�
std::string base64_decode(const std::string& input) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;

    int val = 0, valb = -8;
    std::string result;
    for (unsigned char c : input) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            result.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}


// ��ü ���ڿ��� Base62�� ���ڵ��ϴ� �Լ�
std::string compress(const std::string& input) {
    return base64_encode(input);
}

// Base62�� ���ڵ��� ���ڿ��� ���ڵ��ϴ� �Լ�
std::string decompress(const std::string& encoded) {
    return base64_decode(encoded);
}