#include "zenith/http/multipart.hpp"

namespace zenith::http::multipart {

core::Result<std::string> extractBoundary(const std::string& contentType) {
    // multipart/form-data; boundary=----xyz
    const size_t b = contentType.find("boundary=");
    if (b == std::string::npos) {
        return core::Result<std::string>::Err(core::Error(110, "no boundary in Content-Type"));
    }
    size_t start = b + 9;
    std::string out;
    if (start < contentType.size() && contentType[start] == '"') {
        const size_t e = contentType.find('"', start + 1);
        if (e == std::string::npos) {
            return core::Result<std::string>::Err(core::Error(110, "unterminated boundary quote"));
        }
        out = contentType.substr(start + 1, e - start - 1);
    } else {
        size_t e = contentType.find(';', start);
        if (e == std::string::npos) e = contentType.size();
        out = contentType.substr(start, e - start);
    }
    if (out.empty()) {
        return core::Result<std::string>::Err(core::Error(110, "empty boundary"));
    }
    return core::Result<std::string>::Ok(std::move(out));
}

std::optional<std::string> dispositionParam(const std::string& line, const std::string& key) {
    const std::string pat = key + "=\"";
    const size_t p = line.find(pat);
    if (p == std::string::npos) return std::nullopt;
    const size_t e = line.find('"', p + pat.size());
    if (e == std::string::npos) return std::nullopt;
    return line.substr(p + pat.size(), e - (p + pat.size()));
}

core::Result<std::vector<FormPart>> parse(const std::string& body, const std::string& boundary) {
    const std::string delim = "--" + boundary;
    std::vector<FormPart> parts;

    // 定位首个边界
    size_t pos = body.find(delim);
    if (pos == std::string::npos) {
        return core::Result<std::vector<FormPart>>::Err(core::Error(111, "first boundary missing"));
    }
    pos += delim.size();

    for (;;) {
        // 边界后："--" = 结束；CRLF = 下一 part
        if (body.compare(pos, 2, "--") == 0) break; // 终止边界
        if (body.compare(pos, 2, "\r\n") != 0) {
            return core::Result<std::vector<FormPart>>::Err(
                core::Error(111, "malformed boundary terminator"));
        }
        pos += 2;

        // part 头（至空行）
        const size_t headEnd = body.find("\r\n\r\n", pos);
        if (headEnd == std::string::npos) {
            return core::Result<std::vector<FormPart>>::Err(core::Error(111, "part headers unterminated"));
        }
        const std::string head = body.substr(pos, headEnd - pos);

        FormPart part;
        // 逐行头
        size_t hl = 0;
        while (hl < head.size()) {
            size_t e = head.find("\r\n", hl);
            if (e == std::string::npos) e = head.size();
            const std::string line = head.substr(hl, e - hl);
            hl = e + 2;
            if (line.compare(0, 19, "Content-Disposition") == 0) {
                if (auto n = dispositionParam(line, "name")) part.name = *n;
                if (auto f = dispositionParam(line, "filename")) part.filename = *f;
            } else if (line.compare(0, 13, "Content-Type:") == 0) {
                part.contentType = line.substr(13);
                const size_t sp = part.contentType.find_first_not_of(" \t");
                part.contentType = part.contentType.substr(sp == std::string::npos ? 0 : sp);
            }
        }

        // 数据至下一边界
        const size_t dataBegin = headEnd + 4;
        const size_t next = body.find("\r\n" + delim, dataBegin);
        if (next == std::string::npos) {
            return core::Result<std::vector<FormPart>>::Err(
                core::Error(111, "closing boundary missing for part '" + part.name + "'"));
        }
        part.data = body.substr(dataBegin, next - dataBegin);
        pos = next + 2 + delim.size();
        parts.push_back(std::move(part));
    }
    return core::Result<std::vector<FormPart>>::Ok(std::move(parts));
}

} // namespace zenith::http::multipart
